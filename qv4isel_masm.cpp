
#include "qv4isel_masm_p.h"
#include "qmljs_runtime.h"
#include "qmljs_objects.h"

#include <assembler/LinkBuffer.h>

#include <sys/mman.h>
#include <iostream>
#include <cassert>

#ifndef NO_UDIS86
#  include <udis86.h>
#endif

using namespace QQmlJS;
using namespace QQmlJS::MASM;
using namespace QQmlJS::VM;

namespace {
QTextStream qout(stdout, QIODevice::WriteOnly);
}

InstructionSelection::InstructionSelection(VM::ExecutionEngine *engine, IR::Module *module, uchar *buffer)
    : _engine(engine)
    , _module(module)
    , _function(0)
    , _block(0)
    , _buffer(buffer)
    , _code(buffer)
    , _codePtr(buffer)
{
}

InstructionSelection::~InstructionSelection()
{
}

void InstructionSelection::operator()(IR::Function *function)
{
    qSwap(_function, function);

    int locals = (_function->tempCount - _function->locals.size() + _function->maxNumberOfArguments) * sizeof(Value);
    locals = (locals + 15) & ~15;
    enterStandardStackFrame(locals);

    push(ContextRegister);
    loadPtr(addressForArgument(0), ContextRegister);

    foreach (IR::BasicBlock *block, _function->basicBlocks) {
        _block = block;
        _addrs[block] = label();
        foreach (IR::Stmt *s, block->statements) {
            s->accept(this);
        }
    }

    pop(ContextRegister);

    leaveStandardStackFrame(locals);
    ret();

    QHashIterator<IR::BasicBlock *, QVector<Jump> > it(_patches);
    while (it.hasNext()) {
        it.next();
        IR::BasicBlock *block = it.key();
        Label target = _addrs.value(block);
        assert(target.isSet());
        foreach (Jump jump, it.value())
            jump.linkTo(target, this);
    }

    JSC::JSGlobalData dummy;
    JSC::LinkBuffer linkBuffer(dummy, this, 0);
    foreach (CallToLink ctl, _callsToLink)
        linkBuffer.link(ctl.call, ctl.externalFunction);
    _function->codeRef = linkBuffer.finalizeCodeWithDisassembly("operator()(IR::Function*)");
    _function->code = (void (*)(VM::Context *, const uchar *)) _function->codeRef.code().executableAddress();

    qSwap(_function, function);
}

String *InstructionSelection::identifier(const QString &s)
{
    return _engine->identifier(s);
}

JSC::MacroAssembler::Address InstructionSelection::loadTempAddress(RegisterID reg, IR::Temp *t)
{
    int32_t offset = 0;
    if (t->index < 0) {
        const int arg = -t->index - 1;
        loadPtr(Address(ContextRegister, offsetof(Context, arguments)), reg);
        offset = arg * sizeof(Value);
    } else if (t->index < _function->locals.size()) {
        loadPtr(Address(ContextRegister, offsetof(Context, locals)), reg);
        offset = t->index * sizeof(Value);
    } else {
        const int arg = _function->maxNumberOfArguments + t->index - _function->locals.size();
        offset = sizeof(Value) * (-arg - 1)
                 - sizeof(void*); // size of ebp
        reg = StackFrameRegister;
    }
    return Address(reg, offset);
}

void InstructionSelection::callActivationProperty(IR::Call *call, IR::Temp *result)
{
    IR::Name *baseName = call->base->asName();
    assert(baseName != 0);

    FunctionCall fct(this);
    if (baseName->id)
        fct.callRuntimeMethod(__qmljs_call_activation_property, result, call->base, call->args);
    else {
        switch (baseName->builtin) {
        case IR::Name::builtin_invalid:
            Q_UNREACHABLE();
            break;
        case IR::Name::builtin_typeof:
            fct.callRuntimeMethod(__qmljs_builtin_typeof, result, call->args);
            break;
        case IR::Name::builtin_delete:
            Q_UNREACHABLE();
            break;
        case IR::Name::builtin_throw:
            fct.callRuntimeMethod(__qmljs_builtin_throw, result, call->args);
            break;
        case IR::Name::builtin_rethrow:
            fct.callRuntimeMethod(__qmljs_builtin_rethrow, result, call->args);
            return; // we need to return to avoid checking the exceptions
        }
    }
}


void InstructionSelection::callValue(IR::Call *call, IR::Temp *result)
{
}

void InstructionSelection::callProperty(IR::Call *call, IR::Temp *result)
{
}

void InstructionSelection::constructActivationProperty(IR::New *call, IR::Temp *result)
{
    IR::Name *baseName = call->base->asName();
    assert(baseName != 0);

    FunctionCall fct(this);
    fct.callRuntimeMethod(__qmljs_construct_activation_property, result, call->base, call->args);
}

void InstructionSelection::constructProperty(IR::New *call, IR::Temp *result)
{
    Q_UNIMPLEMENTED();
}

void InstructionSelection::constructValue(IR::New *call, IR::Temp *result)
{
    Q_UNIMPLEMENTED();
}

void InstructionSelection::checkExceptions()
{
    Address addr(ContextRegister, offsetof(Context, hasUncaughtException));
    Jump jmp = branch8(Equal, addr, TrustedImm32(1));
    _patches[_function->handlersBlock].append(jmp);
}

void InstructionSelection::visitExp(IR::Exp *s)
{
    Q_UNIMPLEMENTED();
    assert(!"TODO");
}

void InstructionSelection::visitEnter(IR::Enter *)
{
    Q_UNIMPLEMENTED();
    assert(!"TODO");
}

void InstructionSelection::visitLeave(IR::Leave *)
{
    Q_UNIMPLEMENTED();
    assert(!"TODO");
}

void InstructionSelection::visitMove(IR::Move *s)
{
    if (s->op == IR::OpInvalid) {
        if (IR::Name *n = s->target->asName()) {
            String *propertyName = identifier(*n->id);

            if (IR::Temp *t = s->source->asTemp()) {
                FunctionCall fct(this);
                fct.addArgumentFromRegister(ContextRegister);
                move(TrustedImmPtr(propertyName), Gpr1);
                fct.addArgumentFromRegister(Gpr1);
                fct.addArgumentAsAddress(loadTempAddress(Gpr2, t));
                fct.call(__qmljs_set_activation_property);
                checkExceptions();
                return;
            } else {
                Q_UNREACHABLE();
            }
        } else if (IR::Temp *t = s->target->asTemp()) {
            if (IR::Name *n = s->source->asName()) {
                Address temp = loadTempAddress(Gpr0, t);

                FunctionCall fc(this);
                fc.addArgumentFromRegister(ContextRegister);
                fc.addArgumentAsAddress(temp);

                if (*n->id == QStringLiteral("this")) { // ### `this' should be a builtin.
                    fc.call(__qmljs_get_thisObject);
                } else {
                    String *propertyName = identifier(*n->id);
                    move(TrustedImmPtr(propertyName), Gpr1);
                    fc.addArgumentFromRegister(Gpr1);
                    fc.call(__qmljs_get_activation_property);
                    checkExceptions();
                }
                return;
            } else if (IR::Const *c = s->source->asConst()) {
                Address dest = loadTempAddress(Gpr0, t);
                switch (c->type) {
                case IR::NullType:
                    storeValue<Value::Null_Type>(TrustedImm32(0), dest);
                    break;
                case IR::UndefinedType:
                    storeValue<Value::Undefined_Type>(TrustedImm32(0), dest);
                    break;
                case IR::BoolType:
                    storeValue<Value::Boolean_Type>(TrustedImm32(c->value != 0), dest);
                    break;
                case IR::NumberType:
                    // ### Taking address of pointer inside IR.
                    loadDouble(&c->value, FPGpr0);
                    storeDouble(FPGpr0, dest);
                    break;
                default:
                    Q_UNIMPLEMENTED();
                    assert(!"TODO");
                }
                return;
            } else if (IR::Temp *t2 = s->source->asTemp()) {
                Address dest = loadTempAddress(Gpr1, t);
                Address source = loadTempAddress(Gpr2, t2);
                FunctionCall fc(this);
                fc.addArgumentAsAddress(dest);
                fc.addArgumentAsAddress(source);
                fc.call(__qmljs_copy);
                return;
            } else if (IR::String *str = s->source->asString()) {
                FunctionCall fct(this);
                Address temp = loadTempAddress(Gpr0, t);
                fct.addArgumentAsAddress(temp);
                move(TrustedImmPtr(_engine->newString(*str->value)), Gpr1);
                fct.addArgumentFromRegister(Gpr1);
                fct.call(__qmljs_init_string);
            } else if (IR::Closure *clos = s->source->asClosure()) {
                FunctionCall fct(this);
                fct.addArgumentFromRegister(ContextRegister);
                fct.addArgumentAsAddress(loadTempAddress(Gpr0, t));
                move(TrustedImmPtr(clos->value), Gpr1);
                fct.addArgumentFromRegister(Gpr1);
                fct.call(__qmljs_init_closure);
                return;
            } else if (IR::New *ctor = s->source->asNew()) {
                if (ctor->base->asName()) {
                    constructActivationProperty(ctor, t);
                    return;
                } else if (ctor->base->asMember()) {
                    constructProperty(ctor, t);
                    return;
                } else if (ctor->base->asTemp()) {
                    constructValue(ctor, t);
                    return;
                }
            } else if (IR::Member *m = s->source->asMember()) {
                Q_UNIMPLEMENTED();
            } else if (IR::Subscript *ss = s->source->asSubscript()) {
                Q_UNIMPLEMENTED();
            } else if (IR::Unop *u = s->source->asUnop()) {
                if (IR::Temp *e = u->expr->asTemp()) {
                    FunctionCall fct(this);
                    fct.addArgumentFromRegister(ContextRegister);
                    Address result = loadTempAddress(Gpr1, t);
                    Address value = loadTempAddress(Gpr2, e);
                    fct.addArgumentAsAddress(result);
                    fct.addArgumentAsAddress(value);
                    void (*op)(Context *, Value *, const Value *) = 0;
                    switch (u->op) {
                    case IR::OpIfTrue: assert(!"unreachable"); break;
                    case IR::OpNot: op = __qmljs_not; break;
                    case IR::OpUMinus: op = __qmljs_uminus; break;
                    case IR::OpUPlus: op = __qmljs_uplus; break;
                    case IR::OpCompl: op = __qmljs_compl; break;
                    default: assert(!"unreachable"); break;
                    } // switch
                    fct.call(op);
                    return;
                }
            } else if (IR::Binop *b = s->source->asBinop()) {
                IR::Temp *l = b->left->asTemp();
                IR::Temp *r = b->right->asTemp();
                if (l && r) {
                    FunctionCall fct(this);
                    fct.addArgumentFromRegister(ContextRegister);

                    Address result = loadTempAddress(Gpr1, t);
                    Address lhs = loadTempAddress(Gpr2, l);
                    Address rhs = loadTempAddress(Gpr3, r);
                    fct.addArgumentAsAddress(result);
                    fct.addArgumentAsAddress(lhs);
                    fct.addArgumentAsAddress(rhs);

                    void (*op)(Context *, Value *, const Value *, const Value *) = 0;

                    switch ((IR::AluOp) b->op) {
                    case IR::OpInvalid:
                    case IR::OpIfTrue:
                    case IR::OpNot:
                    case IR::OpUMinus:
                    case IR::OpUPlus:
                    case IR::OpCompl:
                        assert(!"unreachable");
                        break;

                    case IR::OpBitAnd: op = __qmljs_bit_and; break;
                    case IR::OpBitOr: op = __qmljs_bit_or; break;
                    case IR::OpBitXor: op = __qmljs_bit_xor; break;
                    case IR::OpAdd: op = __qmljs_add; break;
                    case IR::OpSub: op = __qmljs_sub; break;
                    case IR::OpMul: op = __qmljs_mul; break;
                    case IR::OpDiv: op = __qmljs_div; break;
                    case IR::OpMod: op = __qmljs_mod; break;
                    case IR::OpLShift: op = __qmljs_shl; break;
                    case IR::OpRShift: op = __qmljs_shr; break;
                    case IR::OpURShift: op = __qmljs_ushr; break;
                    case IR::OpGt: op = __qmljs_gt; break;
                    case IR::OpLt: op = __qmljs_lt; break;
                    case IR::OpGe: op = __qmljs_ge; break;
                    case IR::OpLe: op = __qmljs_le; break;
                    case IR::OpEqual: op = __qmljs_eq; break;
                    case IR::OpNotEqual: op = __qmljs_ne; break;
                    case IR::OpStrictEqual: op = __qmljs_se; break;
                    case IR::OpStrictNotEqual: op = __qmljs_sne; break;
                    case IR::OpInstanceof: op = __qmljs_instanceof; break;
                    case IR::OpIn: op = __qmljs_in; break;

                    case IR::OpAnd:
                    case IR::OpOr:
                        assert(!"unreachable");
                        break;
                    }
                    fct.call(op);
                    return;
                }
            } else if (IR::Call *c = s->source->asCall()) {
                if (c->base->asName()) {
                    callActivationProperty(c, t);
                    return;
                } else if (c->base->asMember()) {
                    callProperty(c, t);
                    return;
                } else if (c->base->asTemp()) {
                    callValue(c, t);
                    return;
                }
            }
        } else if (IR::Member *m = s->target->asMember()) {
            Q_UNIMPLEMENTED();
        } else if (IR::Subscript *ss = s->target->asSubscript()) {
            Q_UNIMPLEMENTED();
        }
    } else {
        // inplace assignment, e.g. x += 1, ++x, ...
        if (IR::Temp *t = s->target->asTemp()) {
            if (IR::Temp *t2 = s->source->asTemp()) {
                FunctionCall fct(this);
                fct.addArgumentFromRegister(ContextRegister);
                fct.addArgument(t);
                fct.addArgument(t2);
                void (*op)(Context *, Value *, const Value *, const Value *) = 0;
                switch (s->op) {
                case IR::OpBitAnd: op = __qmljs_bit_and; break;
                case IR::OpBitOr: op = __qmljs_bit_or; break;
                case IR::OpBitXor: op = __qmljs_bit_xor; break;
                case IR::OpAdd: op = __qmljs_add; break;
                case IR::OpSub: op = __qmljs_sub; break;
                case IR::OpMul: op = __qmljs_mul; break;
                case IR::OpDiv: op = __qmljs_div; break;
                case IR::OpMod: op = __qmljs_mod; break;
                case IR::OpLShift: op = __qmljs_shl; break;
                case IR::OpRShift: op = __qmljs_shr; break;
                case IR::OpURShift: op = __qmljs_ushr; break;
                default:
                    Q_UNREACHABLE();
                    break;
                }

                fct.call(op);
                return;
            }
        } else if (IR::Name *n = s->target->asName()) {
            if (IR::Temp *t = s->source->asTemp()) {
                void (*op)(Context *, String *, Value *) = 0;
                switch (s->op) {
                case IR::OpBitAnd: op = __qmljs_inplace_bit_and_name; break;
                case IR::OpBitOr: op = __qmljs_inplace_bit_or_name; break;
                case IR::OpBitXor: op = __qmljs_inplace_bit_xor_name; break;
                case IR::OpAdd: op = __qmljs_inplace_add_name; break;
                case IR::OpSub: op = __qmljs_inplace_sub_name; break;
                case IR::OpMul: op = __qmljs_inplace_mul_name; break;
                case IR::OpDiv: op = __qmljs_inplace_div_name; break;
                case IR::OpMod: op = __qmljs_inplace_mod_name; break;
                case IR::OpLShift: op = __qmljs_inplace_shl_name; break;
                case IR::OpRShift: op = __qmljs_inplace_shr_name; break;
                case IR::OpURShift: op = __qmljs_inplace_ushr_name; break;
                default:
                    Q_UNREACHABLE();
                    break;
                }
                FunctionCall fct(this);
                fct.addArgumentFromRegister(ContextRegister);
                move(TrustedImmPtr(identifier(*n->id)), Gpr1);
                fct.addArgumentFromRegister(Gpr1);
                fct.addArgument(t);
                fct.call(op);
                checkExceptions();
                return;
            }
        }
        Q_UNIMPLEMENTED();
    }
    Q_UNIMPLEMENTED();
    s->dump(qout, IR::Stmt::MIR);
    qout << endl;
    assert(!"TODO");
}

void InstructionSelection::visitJump(IR::Jump *s)
{
    jumpToBlock(s->target);
}

void InstructionSelection::jumpToBlock(IR::BasicBlock *target)
{
    if (_block->index + 1 != target->index)
        _patches[target].append(jump());
}

void InstructionSelection::visitCJump(IR::CJump *s)
{
    if (IR::Temp *t = s->cond->asTemp()) {
        Address temp = loadTempAddress(Gpr1, t);
        Address tag = temp;
        tag.offset += offsetof(VM::ValueData, tag);
        Jump booleanConversion = branch32(NotEqual, tag, TrustedImm32(VM::Value::Boolean_Type));

        Address data = temp;
        data.offset += offsetof(VM::ValueData, b);
        load32(data, Gpr1);
        Jump testBoolean = jump();

        booleanConversion.link(this);
        {
            FunctionCall fct(this);
            fct.addArgumentFromRegister(ContextRegister);
            fct.addArgumentAsAddress(temp);
            fct.call(__qmljs_to_boolean);
            move(ReturnValueRegister, Gpr1);
        }

        testBoolean.link(this);
        move(TrustedImm32(1), Gpr0);
        Jump target = branch32(Equal, Gpr1, Gpr0);
        _patches[s->iftrue].append(target);

        jumpToBlock(s->iffalse);
        return;
    } else if (IR::Binop *b = s->cond->asBinop()) {
        IR::Temp *l = b->left->asTemp();
        IR::Temp *r = b->right->asTemp();
        if (l && r) {
            Address lhs = loadTempAddress(Gpr1, l);
            Address rhs = loadTempAddress(Gpr2, r);

            bool (*op)(Context *, const Value *, const Value *);
            switch (b->op) {
            default: Q_UNREACHABLE(); assert(!"todo"); break;
            case IR::OpGt: op = __qmljs_cmp_gt; break;
            case IR::OpLt: op = __qmljs_cmp_lt; break;
            case IR::OpGe: op = __qmljs_cmp_ge; break;
            case IR::OpLe: op = __qmljs_cmp_le; break;
            case IR::OpEqual: op = __qmljs_cmp_eq; break;
            case IR::OpNotEqual: op = __qmljs_cmp_ne; break;
            case IR::OpStrictEqual: op = __qmljs_cmp_se; break;
            case IR::OpStrictNotEqual: op = __qmljs_cmp_sne; break;
            case IR::OpInstanceof: op = __qmljs_cmp_instanceof; break;
            case IR::OpIn: op = __qmljs_cmp_in; break;
            } // switch

            FunctionCall fct(this);
            fct.addArgumentFromRegister(ContextRegister);
            fct.addArgumentAsAddress(lhs);
            fct.addArgumentAsAddress(rhs);
            fct.call(op);
            move(ReturnValueRegister, Gpr0);

            move(TrustedImm32(1), Gpr1);
            Jump target = branch32(Equal, Gpr0, Gpr1);
            _patches[s->iftrue].append(target);

            jumpToBlock(s->iffalse);
            return;
        } else {
            assert(!"wip");
        }
        Q_UNIMPLEMENTED();
    }
    Q_UNIMPLEMENTED();
    assert(!"TODO");
}

void InstructionSelection::visitRet(IR::Ret *s)
{
    if (IR::Temp *t = s->expr->asTemp()) {
        Address source = loadTempAddress(Gpr0, t);
        Address result = Address(ContextRegister, offsetof(Context, result));

        FunctionCall fc(this);
        fc.addArgumentAsAddress(result);
        fc.addArgumentAsAddress(source);
        fc.call(__qmljs_copy);
        return;
    }
    Q_UNIMPLEMENTED();
    Q_UNUSED(s);
}

int InstructionSelection::FunctionCall::prepareVariableArguments(IR::ExprList* args)
{
    int argc = 0;
    for (IR::ExprList *it = args; it; it = it->next) {
        ++argc;
    }

    int i = 0;
    for (IR::ExprList *it = args; it; it = it->next, ++i) {
        IR::Temp *arg = it->expr->asTemp();
        assert(arg != 0);

        Address tempAddress = isel->loadTempAddress(Gpr0, arg);
        FunctionCall fc(isel);
        fc.addArgumentAsAddress(isel->argumentAddressForCall(i));
        fc.addArgumentAsAddress(tempAddress);
        fc.call(__qmljs_copy);
    }

    return argc;
}

void InstructionSelection::FunctionCall::callRuntimeMethod(ActivationMethod method, IR::Temp *result, IR::Expr *base, IR::ExprList *args)
{
    IR::Name *baseName = base->asName();
    assert(baseName != 0);

    int argc = prepareVariableArguments(args);

    addArgumentFromRegister(ContextRegister);

    if (result) {
        addArgumentAsAddress(isel->loadTempAddress(Gpr1, result));
    } else {
        isel->xor32(Gpr1, Gpr1);
        addArgumentFromRegister(Gpr1);
    }

    isel->move(TrustedImmPtr(isel->identifier(*baseName->id)), Gpr2);
    addArgumentFromRegister(Gpr2);
    addArgumentAsAddress(isel->baseAddressForCallArguments());
    addArgumentByValue(TrustedImm32(argc));
    call(method);

    isel->checkExceptions();
}

void InstructionSelection::FunctionCall::callRuntimeMethod(BuiltinMethod method, IR::Temp *result, IR::ExprList *args)
{
    int argc = prepareVariableArguments(args);

    addArgumentFromRegister(ContextRegister);

    if (result) {
        addArgumentAsAddress(isel->loadTempAddress(Gpr1, result));
    } else {
        isel->xor32(Gpr1, Gpr1);
        addArgumentFromRegister(Gpr1);
    }

    addArgumentAsAddress(isel->baseAddressForCallArguments());
    addArgumentByValue(TrustedImm32(argc));
    call(method);

    isel->checkExceptions();
}
