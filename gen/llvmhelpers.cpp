#include "gen/llvm.h"

#include "mars.h"
#include "init.h"

#include "gen/tollvm.h"
#include "gen/llvmhelpers.h"
#include "gen/irstate.h"
#include "gen/runtime.h"
#include "gen/logger.h"
#include "gen/arrays.h"
#include "gen/dvalue.h"
#include "gen/complex.h"
#include "gen/classes.h"
#include "gen/functions.h"
#include "gen/typeinf.h"
#include "gen/todebug.h"

/****************************************************************************************/
/*////////////////////////////////////////////////////////////////////////////////////////
// DYNAMIC MEMORY HELPERS
////////////////////////////////////////////////////////////////////////////////////////*/

LLValue* DtoNew(Type* newtype)
{
    // get runtime function
    llvm::Function* fn = LLVM_D_GetRuntimeFunction(gIR->module, "_d_allocmemoryT");
    // get type info
    LLConstant* ti = DtoTypeInfoOf(newtype);
    assert(isaPointer(ti));
    // call runtime allocator
    LLValue* mem = gIR->ir->CreateCall(fn, ti, ".gc_mem");
    // cast
    return DtoBitCast(mem, getPtrToType(DtoType(newtype)), ".gc_mem");
}

void DtoDeleteMemory(LLValue* ptr)
{
    // get runtime function
    llvm::Function* fn = LLVM_D_GetRuntimeFunction(gIR->module, "_d_delmemory");
    // build args
    LLSmallVector<LLValue*,1> arg;
    arg.push_back(DtoBitCast(ptr, getVoidPtrType(), ".tmp"));
    // call
    llvm::CallInst::Create(fn, arg.begin(), arg.end(), "", gIR->scopebb());
}

void DtoDeleteClass(LLValue* inst)
{
    // get runtime function
    llvm::Function* fn = LLVM_D_GetRuntimeFunction(gIR->module, "_d_delclass");
    // build args
    LLSmallVector<LLValue*,1> arg;
    arg.push_back(DtoBitCast(inst, fn->getFunctionType()->getParamType(0), ".tmp"));
    // call
    llvm::CallInst::Create(fn, arg.begin(), arg.end(), "", gIR->scopebb());
}

void DtoDeleteInterface(LLValue* inst)
{
    // get runtime function
    llvm::Function* fn = LLVM_D_GetRuntimeFunction(gIR->module, "_d_delinterface");
    // build args
    LLSmallVector<LLValue*,1> arg;
    arg.push_back(DtoBitCast(inst, fn->getFunctionType()->getParamType(0), ".tmp"));
    // call
    llvm::CallInst::Create(fn, arg.begin(), arg.end(), "", gIR->scopebb());
}

void DtoDeleteArray(DValue* arr)
{
    // get runtime function
    llvm::Function* fn = LLVM_D_GetRuntimeFunction(gIR->module, "_d_delarray");
    // build args
    LLSmallVector<LLValue*,2> arg;
    arg.push_back(DtoArrayLen(arr));
    arg.push_back(DtoBitCast(DtoArrayPtr(arr), getVoidPtrType(), ".tmp"));
    // call
    llvm::CallInst::Create(fn, arg.begin(), arg.end(), "", gIR->scopebb());
}

/****************************************************************************************/
/*////////////////////////////////////////////////////////////////////////////////////////
// ASSERT HELPER
////////////////////////////////////////////////////////////////////////////////////////*/

void DtoAssert(Loc* loc, DValue* msg)
{
    std::vector<LLValue*> args;
    LLConstant* c;

    // func
    const char* fname = msg ? "_d_assert_msg" : "_d_assert";
    llvm::Function* fn = LLVM_D_GetRuntimeFunction(gIR->module, fname);

    // param attrs
    llvm::PAListPtr palist;
    int idx = 1;

    c = DtoConstString(loc->filename);

    // msg param
    if (msg)
    {
        if (DSliceValue* s = msg->isSlice())
        {
            llvm::AllocaInst* alloc = gIR->func()->msgArg;
            if (!alloc)
            {
                alloc = new llvm::AllocaInst(c->getType(), ".assertmsg", gIR->topallocapoint());
                DtoSetArray(alloc, DtoArrayLen(s), DtoArrayPtr(s));
                gIR->func()->msgArg = alloc;
            }
            args.push_back(alloc);
        }
        else
        {
            args.push_back(msg->getRVal());
        }
        palist = palist.addAttr(idx++, llvm::ParamAttr::ByVal);
    }

    // file param
    llvm::AllocaInst* alloc = gIR->func()->srcfileArg;
    if (!alloc)
    {
        alloc = new llvm::AllocaInst(c->getType(), ".srcfile", gIR->topallocapoint());
        gIR->func()->srcfileArg = alloc;
    }
    LLValue* ptr = DtoGEPi(alloc, 0,0, "tmp");
    DtoStore(c->getOperand(0), ptr);
    ptr = DtoGEPi(alloc, 0,1, "tmp");
    DtoStore(c->getOperand(1), ptr);

    args.push_back(alloc);
    palist = palist.addAttr(idx++, llvm::ParamAttr::ByVal);


    // line param
    c = DtoConstUint(loc->linnum);
    args.push_back(c);

    // call
    llvm::CallInst* call = llvm::CallInst::Create(fn, args.begin(), args.end(), "", gIR->scopebb());
    call->setParamAttrs(palist);
}

/****************************************************************************************/
/*////////////////////////////////////////////////////////////////////////////////////////
// NESTED VARIABLE HELPERS
////////////////////////////////////////////////////////////////////////////////////////*/

static const LLType* get_next_frame_ptr_type(Dsymbol* sc)
{
    assert(sc->isFuncDeclaration() || sc->isClassDeclaration());
    Dsymbol* p = sc->toParent2();
    if (!p->isFuncDeclaration() && !p->isClassDeclaration())
        Logger::println("unexpected parent symbol found while resolving frame pointer - '%s' kind: '%s'", p->toChars(), p->kind());
    assert(p->isFuncDeclaration() || p->isClassDeclaration());
    if (FuncDeclaration* fd = p->isFuncDeclaration())
    {
        LLValue* v = fd->ir.irFunc->nestedVar;
        assert(v);
        return v->getType();
    }
    else if (ClassDeclaration* cd = p->isClassDeclaration())
    {
        return DtoType(cd->type);
    }
    else
    {
        Logger::println("symbol: '%s' kind: '%s'", sc->toChars(), sc->kind());
        assert(0);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

static LLValue* get_frame_ptr_impl(FuncDeclaration* func, Dsymbol* sc, LLValue* v)
{
    LOG_SCOPE;
    if (sc == func)
    {
        return v;
    }
    else if (FuncDeclaration* fd = sc->isFuncDeclaration())
    {
        Logger::println("scope is function: %s", fd->toChars());

        if (fd->toParent2() == func)
        {
            if (!func->ir.irFunc->nestedVar)
                return NULL;
            return DtoBitCast(v, func->ir.irFunc->nestedVar->getType());
        }

        v = DtoBitCast(v, get_next_frame_ptr_type(fd));
        Logger::cout() << "v = " << *v << '\n';

        if (fd->toParent2()->isFuncDeclaration())
        {
            v = DtoGEPi(v, 0,0, "tmp");
            v = DtoLoad(v);
        }
        else if (ClassDeclaration* cd = fd->toParent2()->isClassDeclaration())
        {
            size_t idx = 2;
            //idx += cd->ir.irStruct->interfaceVec.size();
            v = DtoGEPi(v,0,idx,"tmp");
            v = DtoLoad(v);
        }
        else
        {
            assert(0);
        }
        return get_frame_ptr_impl(func, fd->toParent2(), v);
    }
    else if (ClassDeclaration* cd = sc->isClassDeclaration())
    {
        Logger::println("scope is class: %s", cd->toChars());
        /*size_t idx = 2;
        idx += cd->llvmIrStruct->interfaces.size();
        v = DtoGEPi(v,0,idx,"tmp");
        Logger::cout() << "gep = " << *v << '\n';
        v = DtoLoad(v);*/
        return get_frame_ptr_impl(func, cd->toParent2(), v);
    }
    else
    {
        Logger::println("symbol: '%s'", sc->toPrettyChars());
        assert(0);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

static LLValue* get_frame_ptr(FuncDeclaration* func)
{
    Logger::println("Resolving context pointer for nested function: '%s'", func->toPrettyChars());
    LOG_SCOPE;
    IrFunction* irfunc = gIR->func();

    // in the right scope already
    if (func == irfunc->decl)
        return irfunc->decl->ir.irFunc->nestedVar;

    // use the 'this' pointer
    LLValue* ptr = irfunc->decl->ir.irFunc->thisVar;
    assert(ptr);

    // return the fully resolved frame pointer
    ptr = get_frame_ptr_impl(func, irfunc->decl, ptr);
    if (ptr) Logger::cout() << "Found context!" << *ptr;
    else Logger::cout() << "NULL context!\n";

    return ptr;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoNestedContext(FuncDeclaration* func)
{
    // resolve frame ptr
    LLValue* ptr = get_frame_ptr(func);
    Logger::cout() << "Nested context ptr = ";
    if (ptr) Logger::cout() << *ptr;
    else Logger::cout() << "NULL";
    Logger::cout() << '\n';
    return ptr;
}

//////////////////////////////////////////////////////////////////////////////////////////

static void print_frame_worker(VarDeclaration* vd, Dsymbol* par)
{
    if (vd->toParent2() == par)
    {
        Logger::println("found: '%s' kind: '%s'", par->toChars(), par->kind());
        return;
    }

    Logger::println("diving into: '%s' kind: '%s'", par->toChars(), par->kind());
    LOG_SCOPE;
    print_frame_worker(vd, par->toParent2());
}

//////////////////////////////////////////////////////////////////////////////////////////

static void print_nested_frame_list(VarDeclaration* vd, Dsymbol* par)
{
    Logger::println("Frame pointer list for nested var: '%s'", vd->toPrettyChars());
    LOG_SCOPE;
    if (vd->toParent2() != par)
        print_frame_worker(vd, par);
    else
        Logger::println("Found at level 0");
    Logger::println("Done");
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoNestedVariable(VarDeclaration* vd)
{
    // log the frame list
    IrFunction* irfunc = gIR->func();
    if (Logger::enabled())
        print_nested_frame_list(vd, irfunc->decl);

    // resolve frame ptr
    FuncDeclaration* func = vd->toParent2()->isFuncDeclaration();
    assert(func);
    LLValue* ptr = DtoNestedContext(func);
    assert(ptr && "nested var, but no context");

    // we must cast here to be sure. nested classes just have a void*
    ptr = DtoBitCast(ptr, func->ir.irFunc->nestedVar->getType());

    // index nested var and load (if necessary)
    LLValue* v = DtoGEPi(ptr, 0, vd->ir.irLocal->nestedIndex, "tmp");
    // references must be loaded, for normal variables this IS already the variable storage!!!
    if (vd->isParameter() && (vd->isRef() || vd->isOut() || DtoIsPassedByRef(vd->type)))
        v = DtoLoad(v);

    // log and return
    Logger::cout() << "Nested var ptr = " << *v << '\n';
    return v;
}

/****************************************************************************************/
/*////////////////////////////////////////////////////////////////////////////////////////
// ASSIGNMENT HELPER (store this in that)
////////////////////////////////////////////////////////////////////////////////////////*/

void DtoAssign(DValue* lhs, DValue* rhs)
{
    Logger::cout() << "DtoAssign(...);\n";
    LOG_SCOPE;

    Type* t = DtoDType(lhs->getType());
    Type* t2 = DtoDType(rhs->getType());

    if (t->ty == Tstruct) {
        if (t2 != t) {
            // TODO: fix this, use 'rhs' for something
            DtoAggrZeroInit(lhs->getLVal());
        }
        else if (!rhs->inPlace()) {
            DtoAggrCopy(lhs->getLVal(), rhs->getRVal());
        }
    }
    else if (t->ty == Tarray) {
        // lhs is slice
        if (DSliceValue* s = lhs->isSlice()) {
            if (DSliceValue* s2 = rhs->isSlice()) {
                DtoArrayCopySlices(s, s2);
            }
            else if (t->next == t2) {
                if (s->len)
                    DtoArrayInit(s->ptr, s->len, rhs->getRVal());
                else
                    DtoArrayInit(s->ptr, rhs->getRVal());
            }
            else {
                DtoArrayCopyToSlice(s, rhs);
            }
        }
        // rhs is slice
        else if (DSliceValue* s = rhs->isSlice()) {
            assert(s->getType()->toBasetype() == lhs->getType()->toBasetype());
            DtoSetArray(lhs->getLVal(),DtoArrayLen(s),DtoArrayPtr(s));
        }
        // null
        else if (rhs->isNull()) {
            DtoSetArrayToNull(lhs->getLVal());
        }
        // reference assignment
        else {
            DtoArrayAssign(lhs->getLVal(), rhs->getRVal());
        }
    }
    else if (t->ty == Tsarray) {
        if (DtoType(lhs->getType()) == DtoType(rhs->getType())) {
            DtoStaticArrayCopy(lhs->getLVal(), rhs->getRVal());
        }
        else {
            DtoArrayInit(lhs->getLVal(), rhs->getRVal());
        }
    }
    else if (t->ty == Tdelegate) {
        if (rhs->isNull())
            DtoAggrZeroInit(lhs->getLVal());
        else if (!rhs->inPlace()) {
            LLValue* l = lhs->getLVal();
            LLValue* r = rhs->getRVal();
            Logger::cout() << "assign\nlhs: " << *l << "rhs: " << *r << '\n';
            DtoAggrCopy(l, r);
        }
    }
    else if (t->ty == Tclass) {
        assert(t2->ty == Tclass);
        // assignment to this in constructor special case
        if (lhs->isThis()) {
            LLValue* tmp = rhs->getRVal();
            FuncDeclaration* fdecl = gIR->func()->decl;
            // respecify the this param
            if (!llvm::isa<llvm::AllocaInst>(fdecl->ir.irFunc->thisVar))
                fdecl->ir.irFunc->thisVar = new llvm::AllocaInst(tmp->getType(), "newthis", gIR->topallocapoint());
            DtoStore(tmp, fdecl->ir.irFunc->thisVar);
        }
        // regular class ref -> class ref assignment
        else {
            DtoStore(rhs->getRVal(), lhs->getLVal());
        }
    }
    else if (t->iscomplex()) {
        assert(!lhs->isComplex());

        LLValue* dst;
        if (DLRValue* lr = lhs->isLRValue()) {
            dst = lr->getLVal();
            rhs = DtoCastComplex(rhs, lr->getLType());
        }
        else {
            dst = lhs->getRVal();
        }

        if (DComplexValue* cx = rhs->isComplex())
            DtoComplexSet(dst, cx->re, cx->im);
        else
            DtoComplexAssign(dst, rhs->getRVal());
    }
    else {
        LLValue* l = lhs->getLVal();
        LLValue* r = rhs->getRVal();
        Logger::cout() << "assign\nlhs: " << *l << "rhs: " << *r << '\n';
        const LLType* lit = l->getType()->getContainedType(0);
        if (r->getType() != lit) {
            // handle lvalue cast assignments
            if (DLRValue* lr = lhs->isLRValue()) {
                Logger::println("lvalue cast!");
                r = DtoCast(rhs, lr->getLType())->getRVal();
            }
            else {
                r = DtoCast(rhs, lhs->getType())->getRVal();
            }
            Logger::cout() << "really assign\nlhs: " << *l << "rhs: " << *r << '\n';
            assert(r->getType() == l->getType()->getContainedType(0));
        }
        gIR->ir->CreateStore(r, l);
    }
}

/****************************************************************************************/
/*////////////////////////////////////////////////////////////////////////////////////////
//      CASTING HELPERS
////////////////////////////////////////////////////////////////////////////////////////*/

DValue* DtoCastInt(DValue* val, Type* _to)
{
    const LLType* tolltype = DtoType(_to);

    Type* to = DtoDType(_to);
    Type* from = DtoDType(val->getType());
    assert(from->isintegral());

    size_t fromsz = from->size();
    size_t tosz = to->size();

    LLValue* rval = val->getRVal();
    if (rval->getType() == tolltype) {
        return new DImValue(_to, rval);
    }

    if (to->isintegral()) {
        if (fromsz < tosz) {
            Logger::cout() << "cast to: " << *tolltype << '\n';
            if (from->isunsigned() || from->ty == Tbool) {
                rval = new llvm::ZExtInst(rval, tolltype, "tmp", gIR->scopebb());
            } else {
                rval = new llvm::SExtInst(rval, tolltype, "tmp", gIR->scopebb());
            }
        }
        else if (fromsz > tosz) {
            rval = new llvm::TruncInst(rval, tolltype, "tmp", gIR->scopebb());
        }
        else {
            rval = DtoBitCast(rval, tolltype);
        }
    }
    else if (to->isfloating()) {
        if (from->isunsigned()) {
            rval = new llvm::UIToFPInst(rval, tolltype, "tmp", gIR->scopebb());
        }
        else {
            rval = new llvm::SIToFPInst(rval, tolltype, "tmp", gIR->scopebb());
        }
    }
    else if (to->ty == Tpointer) {
        Logger::cout() << "cast pointer: " << *tolltype << '\n';
        rval = gIR->ir->CreateIntToPtr(rval, tolltype, "tmp");
    }
    else {
        assert(0 && "bad int cast");
    }

    return new DImValue(_to, rval);
}

DValue* DtoCastPtr(DValue* val, Type* to)
{
    const LLType* tolltype = DtoType(to);

    Type* totype = DtoDType(to);
    Type* fromtype = DtoDType(val->getType());
    assert(fromtype->ty == Tpointer || fromtype->ty == Tfunction);

    LLValue* rval;

    if (totype->ty == Tpointer || totype->ty == Tclass) {
        LLValue* src = val->getRVal();
        Logger::cout() << "src: " << *src << "to type: " << *tolltype << '\n';
        rval = DtoBitCast(src, tolltype);
    }
    else if (totype->isintegral()) {
        rval = new llvm::PtrToIntInst(val->getRVal(), tolltype, "tmp", gIR->scopebb());
    }
    else {
        Logger::println("invalid cast from '%s' to '%s'", val->getType()->toChars(), to->toChars());
        assert(0);
    }

    return new DImValue(to, rval);
}

DValue* DtoCastFloat(DValue* val, Type* to)
{
    if (val->getType() == to)
        return val;

    const LLType* tolltype = DtoType(to);

    Type* totype = DtoDType(to);
    Type* fromtype = DtoDType(val->getType());
    assert(fromtype->isfloating());

    size_t fromsz = fromtype->size();
    size_t tosz = totype->size();

    LLValue* rval;

    if (totype->iscomplex()) {
        assert(0);
        //return new DImValue(to, DtoComplex(to, val));
    }
    else if (totype->isfloating()) {
        if ((fromtype->ty == Tfloat80 || fromtype->ty == Tfloat64) && (totype->ty == Tfloat80 || totype->ty == Tfloat64)) {
            rval = val->getRVal();
        }
        else if (fromsz < tosz) {
            rval = new llvm::FPExtInst(val->getRVal(), tolltype, "tmp", gIR->scopebb());
        }
        else if (fromsz > tosz) {
            rval = new llvm::FPTruncInst(val->getRVal(), tolltype, "tmp", gIR->scopebb());
        }
        else {
            assert(0 && "bad float cast");
        }
    }
    else if (totype->isintegral()) {
        if (totype->isunsigned()) {
            rval = new llvm::FPToUIInst(val->getRVal(), tolltype, "tmp", gIR->scopebb());
        }
        else {
            rval = new llvm::FPToSIInst(val->getRVal(), tolltype, "tmp", gIR->scopebb());
        }
    }
    else {
        assert(0 && "bad float cast");
    }

    return new DImValue(to, rval);
}

DValue* DtoCast(DValue* val, Type* to)
{
    Type* fromtype = DtoDType(val->getType());
    Logger::println("Casting from '%s' to '%s'", fromtype->toChars(), to->toChars());
    if (fromtype->isintegral()) {
        return DtoCastInt(val, to);
    }
    else if (fromtype->iscomplex()) {
        return DtoCastComplex(val, to);
    }
    else if (fromtype->isfloating()) {
        return DtoCastFloat(val, to);
    }
    else if (fromtype->ty == Tclass) {
        return DtoCastClass(val, to);
    }
    else if (fromtype->ty == Tarray || fromtype->ty == Tsarray) {
        return DtoCastArray(val, to);
    }
    else if (fromtype->ty == Tpointer || fromtype->ty == Tfunction) {
        return DtoCastPtr(val, to);
    }
    else {
        assert(0);
    }
}

/****************************************************************************************/
/*////////////////////////////////////////////////////////////////////////////////////////
//      TEMPLATE HELPERS
////////////////////////////////////////////////////////////////////////////////////////*/

bool DtoIsTemplateInstance(Dsymbol* s)
{
    if (!s) return false;
    if (s->isTemplateInstance() && !s->isTemplateMixin())
        return true;
    else if (s->parent)
        return DtoIsTemplateInstance(s->parent);
    return false;
}

/****************************************************************************************/
/*////////////////////////////////////////////////////////////////////////////////////////
//      LAZY STATIC INIT HELPER
////////////////////////////////////////////////////////////////////////////////////////*/

void DtoLazyStaticInit(bool istempl, LLValue* gvar, Initializer* init, Type* t)
{
    // create a flag to make sure initialization only happens once
    llvm::GlobalValue::LinkageTypes gflaglink = istempl ? llvm::GlobalValue::WeakLinkage : llvm::GlobalValue::InternalLinkage;
    std::string gflagname(gvar->getName());
    gflagname.append("__initflag");
    llvm::GlobalVariable* gflag = new llvm::GlobalVariable(LLType::Int1Ty,false,gflaglink,DtoConstBool(false),gflagname,gIR->module);

    // check flag and do init if not already done
    llvm::BasicBlock* oldend = gIR->scopeend();
    llvm::BasicBlock* initbb = llvm::BasicBlock::Create("ifnotinit",gIR->topfunc(),oldend);
    llvm::BasicBlock* endinitbb = llvm::BasicBlock::Create("ifnotinitend",gIR->topfunc(),oldend);
    LLValue* cond = gIR->ir->CreateICmpEQ(gIR->ir->CreateLoad(gflag,"tmp"),DtoConstBool(false));
    gIR->ir->CreateCondBr(cond, initbb, endinitbb);
    gIR->scope() = IRScope(initbb,endinitbb);
    DValue* ie = DtoInitializer(init);
    if (!ie->inPlace()) {
        DValue* dst = new DVarValue(t, gvar, true);
        DtoAssign(dst, ie);
    }
    gIR->ir->CreateStore(DtoConstBool(true), gflag);
    gIR->ir->CreateBr(endinitbb);
    gIR->scope() = IRScope(endinitbb,oldend);
}

/****************************************************************************************/
/*////////////////////////////////////////////////////////////////////////////////////////
//      PROCESSING QUEUE HELPERS
////////////////////////////////////////////////////////////////////////////////////////*/

void DtoResolveDsymbol(Dsymbol* dsym)
{
    if (StructDeclaration* sd = dsym->isStructDeclaration()) {
        DtoResolveStruct(sd);
    }
    else if (ClassDeclaration* cd = dsym->isClassDeclaration()) {
        DtoResolveClass(cd);
    }
    else if (FuncDeclaration* fd = dsym->isFuncDeclaration()) {
        DtoResolveFunction(fd);
    }
    else if (TypeInfoDeclaration* fd = dsym->isTypeInfoDeclaration()) {
        DtoResolveTypeInfo(fd);
    }
    else {
    error(dsym->loc, "unsupported dsymbol: %s", dsym->toChars());
    assert(0 && "unsupported dsymbol for DtoResolveDsymbol");
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoDeclareDsymbol(Dsymbol* dsym)
{
    if (StructDeclaration* sd = dsym->isStructDeclaration()) {
        DtoDeclareStruct(sd);
    }
    else if (ClassDeclaration* cd = dsym->isClassDeclaration()) {
        DtoDeclareClass(cd);
    }
    else if (FuncDeclaration* fd = dsym->isFuncDeclaration()) {
        DtoDeclareFunction(fd);
    }
    else if (TypeInfoDeclaration* fd = dsym->isTypeInfoDeclaration()) {
        DtoDeclareTypeInfo(fd);
    }
    else {
    error(dsym->loc, "unsupported dsymbol: %s", dsym->toChars());
    assert(0 && "unsupported dsymbol for DtoDeclareDsymbol");
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoConstInitDsymbol(Dsymbol* dsym)
{
    if (StructDeclaration* sd = dsym->isStructDeclaration()) {
        DtoConstInitStruct(sd);
    }
    else if (ClassDeclaration* cd = dsym->isClassDeclaration()) {
        DtoConstInitClass(cd);
    }
    else if (TypeInfoDeclaration* fd = dsym->isTypeInfoDeclaration()) {
        DtoConstInitTypeInfo(fd);
    }
    else if (VarDeclaration* vd = dsym->isVarDeclaration()) {
        DtoConstInitGlobal(vd);
    }
    else {
    error(dsym->loc, "unsupported dsymbol: %s", dsym->toChars());
    assert(0 && "unsupported dsymbol for DtoConstInitDsymbol");
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoDefineDsymbol(Dsymbol* dsym)
{
    if (StructDeclaration* sd = dsym->isStructDeclaration()) {
        DtoDefineStruct(sd);
    }
    else if (ClassDeclaration* cd = dsym->isClassDeclaration()) {
        DtoDefineClass(cd);
    }
    else if (FuncDeclaration* fd = dsym->isFuncDeclaration()) {
        DtoDefineFunc(fd);
    }
    else if (TypeInfoDeclaration* fd = dsym->isTypeInfoDeclaration()) {
        DtoDefineTypeInfo(fd);
    }
    else {
    error(dsym->loc, "unsupported dsymbol: %s", dsym->toChars());
    assert(0 && "unsupported dsymbol for DtoDefineDsymbol");
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoConstInitGlobal(VarDeclaration* vd)
{
    if (vd->ir.initialized) return;
    vd->ir.initialized = gIR->dmodule;

    Logger::println("* DtoConstInitGlobal(%s)", vd->toChars());
    LOG_SCOPE;

    bool emitRTstaticInit = false;

    LLConstant* _init = 0;
    if (vd->parent && vd->parent->isFuncDeclaration() && vd->init && vd->init->isExpInitializer()) {
        _init = DtoConstInitializer(vd->type, NULL);
        emitRTstaticInit = true;
    }
    else {
        _init = DtoConstInitializer(vd->type, vd->init);
    }

    const LLType* _type = DtoType(vd->type);
    Type* t = DtoDType(vd->type);

    //Logger::cout() << "initializer: " << *_init << '\n';
    if (_type != _init->getType()) {
        Logger::cout() << "got type '" << *_init->getType() << "' expected '" << *_type << "'\n";
        // zero initalizer
        if (_init->isNullValue())
            _init = llvm::Constant::getNullValue(_type);
        // pointer to global constant (struct.init)
        else if (llvm::isa<llvm::GlobalVariable>(_init))
        {
            assert(_init->getType()->getContainedType(0) == _type);
            llvm::GlobalVariable* gv = llvm::cast<llvm::GlobalVariable>(_init);
            assert(t->ty == Tstruct);
            TypeStruct* ts = (TypeStruct*)t;
            assert(ts->sym->ir.irStruct->constInit);
            _init = ts->sym->ir.irStruct->constInit;
        }
        // array single value init
        else if (isaArray(_type))
        {
            _init = DtoConstStaticArray(_type, _init);
        }
        else {
            Logger::cout() << "Unexpected initializer type: " << *_type << '\n';
            //assert(0);
        }
    }

    bool istempl = false;
    if ((vd->storage_class & STCcomdat) || (vd->parent && DtoIsTemplateInstance(vd->parent))) {
        istempl = true;
    }

    if (_init && _init->getType() != _type)
        _type = _init->getType();
    llvm::cast<LLOpaqueType>(vd->ir.irGlobal->type.get())->refineAbstractTypeTo(_type);
    _type = vd->ir.irGlobal->type.get();
    //_type->dump();
    assert(!_type->isAbstract());

    llvm::GlobalVariable* gvar = llvm::cast<llvm::GlobalVariable>(vd->ir.irGlobal->value);
    if (!(vd->storage_class & STCextern) && (vd->getModule() == gIR->dmodule || istempl))
    {
        gvar->setInitializer(_init);
        // do debug info
        if (global.params.symdebug)
        {
            LLGlobalVariable* gv = DtoDwarfGlobalVariable(gvar, vd);
            // keep a reference so GDCE doesn't delete it !
            gIR->usedArray.push_back(llvm::ConstantExpr::getBitCast(gv, getVoidPtrType()));
        }
    }

    if (emitRTstaticInit)
        DtoLazyStaticInit(istempl, gvar, vd->init, t);
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoEmptyResolveList()
{
    //Logger::println("DtoEmptyResolveList()");
    Dsymbol* dsym;
    while (!gIR->resolveList.empty()) {
        dsym = gIR->resolveList.front();
        gIR->resolveList.pop_front();
        DtoResolveDsymbol(dsym);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoEmptyDeclareList()
{
    //Logger::println("DtoEmptyDeclareList()");
    Dsymbol* dsym;
    while (!gIR->declareList.empty()) {
        dsym = gIR->declareList.front();
        gIR->declareList.pop_front();
        DtoDeclareDsymbol(dsym);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoEmptyConstInitList()
{
    //Logger::println("DtoEmptyConstInitList()");
    Dsymbol* dsym;
    while (!gIR->constInitList.empty()) {
        dsym = gIR->constInitList.front();
        gIR->constInitList.pop_front();
        DtoConstInitDsymbol(dsym);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoEmptyDefineList()
{
    //Logger::println("DtoEmptyDefineList()");
    Dsymbol* dsym;
    while (!gIR->defineList.empty()) {
        dsym = gIR->defineList.front();
        gIR->defineList.pop_front();
        DtoDefineDsymbol(dsym);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
void DtoEmptyAllLists()
{
    for(;;)
    {
        Dsymbol* dsym;
        if (!gIR->resolveList.empty()) {
            dsym = gIR->resolveList.front();
            gIR->resolveList.pop_front();
            DtoResolveDsymbol(dsym);
        }
        else if (!gIR->declareList.empty()) {
            dsym = gIR->declareList.front();
            gIR->declareList.pop_front();
            DtoDeclareDsymbol(dsym);
        }
        else if (!gIR->constInitList.empty()) {
            dsym = gIR->constInitList.front();
            gIR->constInitList.pop_front();
            DtoConstInitDsymbol(dsym);
        }
        else if (!gIR->defineList.empty()) {
            dsym = gIR->defineList.front();
            gIR->defineList.pop_front();
            DtoDefineDsymbol(dsym);
        }
        else {
            break;
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoForceDeclareDsymbol(Dsymbol* dsym)
{
    if (dsym->ir.declared) return;
    Logger::println("DtoForceDeclareDsymbol(%s)", dsym->toPrettyChars());
    LOG_SCOPE;
    DtoResolveDsymbol(dsym);

    DtoEmptyResolveList();

    DtoDeclareDsymbol(dsym);
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoForceConstInitDsymbol(Dsymbol* dsym)
{
    if (dsym->ir.initialized) return;
    Logger::println("DtoForceConstInitDsymbol(%s)", dsym->toPrettyChars());
    LOG_SCOPE;
    DtoResolveDsymbol(dsym);

    DtoEmptyResolveList();
    DtoEmptyDeclareList();

    DtoConstInitDsymbol(dsym);
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoForceDefineDsymbol(Dsymbol* dsym)
{
    if (dsym->ir.defined) return;
    Logger::println("DtoForceDefineDsymbol(%s)", dsym->toPrettyChars());
    LOG_SCOPE;
    DtoResolveDsymbol(dsym);

    DtoEmptyResolveList();
    DtoEmptyDeclareList();
    DtoEmptyConstInitList();

    DtoDefineDsymbol(dsym);
}

/****************************************************************************************/
/*////////////////////////////////////////////////////////////////////////////////////////
//      INITIALIZER HELPERS
////////////////////////////////////////////////////////////////////////////////////////*/

LLConstant* DtoConstInitializer(Type* type, Initializer* init)
{
    LLConstant* _init = 0; // may return zero
    if (!init)
    {
        Logger::println("const default initializer for %s", type->toChars());
        _init = type->defaultInit()->toConstElem(gIR);
    }
    else if (ExpInitializer* ex = init->isExpInitializer())
    {
        Logger::println("const expression initializer");
        _init = ex->exp->toConstElem(gIR);
    }
    else if (StructInitializer* si = init->isStructInitializer())
    {
        Logger::println("const struct initializer");
        _init = DtoConstStructInitializer(si);
    }
    else if (ArrayInitializer* ai = init->isArrayInitializer())
    {
        Logger::println("const array initializer");
        _init = DtoConstArrayInitializer(ai);
    }
    else if (init->isVoidInitializer())
    {
        Logger::println("const void initializer");
        const LLType* ty = DtoType(type);
        _init = llvm::Constant::getNullValue(ty);
    }
    else {
        Logger::println("unsupported const initializer: %s", init->toChars());
    }
    return _init;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* DtoConstFieldInitializer(Type* t, Initializer* init)
{
    Logger::println("DtoConstFieldInitializer");
    LOG_SCOPE;

    const LLType* _type = DtoType(t);

    LLConstant* _init = DtoConstInitializer(t, init);
    assert(_init);
    if (_type != _init->getType())
    {
        Logger::cout() << "field init is: " << *_init << " type should be " << *_type << '\n';
        if (t->ty == Tsarray)
        {
            const LLArrayType* arrty = isaArray(_type);
            uint64_t n = arrty->getNumElements();
            std::vector<LLConstant*> vals(n,_init);
            _init = llvm::ConstantArray::get(arrty, vals);
        }
        else if (t->ty == Tarray)
        {
            assert(isaStruct(_type));
            _init = llvm::ConstantAggregateZero::get(_type);
        }
        else if (t->ty == Tstruct)
        {
            const LLStructType* structty = isaStruct(_type);
            TypeStruct* ts = (TypeStruct*)t;
            assert(ts);
            assert(ts->sym);
            assert(ts->sym->ir.irStruct->constInit);
            _init = ts->sym->ir.irStruct->constInit;
        }
        else if (t->ty == Tclass)
        {
            _init = llvm::Constant::getNullValue(_type);
        }
        else {
            Logger::println("failed for type %s", t->toChars());
            assert(0);
        }
    }

    return _init;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DtoInitializer(Initializer* init)
{
    if (ExpInitializer* ex = init->isExpInitializer())
    {
        Logger::println("expression initializer");
        assert(ex->exp);
        return ex->exp->toElem(gIR);
    }
    else if (init->isVoidInitializer())
    {
        // do nothing
    }
    else {
        Logger::println("unsupported initializer: %s", init->toChars());
        assert(0);
    }
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////////////

void DtoAnnotation(const char* str)
{
    std::string s("CODE: ");
    s.append(str);
    char* p = &s[0];
    while (*p)
    {
        if (*p == '"')
            *p = '\'';
        ++p;
    }
    // create a noop with the code as the result name!
    gIR->ir->CreateAnd(DtoConstSize_t(0),DtoConstSize_t(0),s.c_str());
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* DtoTypeInfoOf(Type* type, bool base)
{
    const LLType* typeinfotype = DtoType(Type::typeinfo->type);
    if (!type->vtinfo)
        type->getTypeInfo(NULL);
    TypeInfoDeclaration* tidecl = type->vtinfo;
    DtoForceDeclareDsymbol(tidecl);
    assert(tidecl->ir.irGlobal != NULL);
    LLConstant* c = isaConstant(tidecl->ir.irGlobal->value);
    assert(c != NULL);
    if (base)
        return llvm::ConstantExpr::getBitCast(c, typeinfotype);
    return c;
}