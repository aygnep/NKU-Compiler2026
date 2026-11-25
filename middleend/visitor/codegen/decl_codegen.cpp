#include <middleend/visitor/codegen/ast_codegen.h>

namespace ME
{
    void ASTCodeGen::visit(FE::AST::Initializer& node, Module* m) { apply(*this, *node.init_val, m); }
    void ASTCodeGen::visit(FE::AST::InitializerList& node, Module* m)
    { if (node.init_list) for (auto* i : *node.init_list) apply(*this, *i, m); }
    void ASTCodeGen::visit(FE::AST::VarDeclarator& node, Module* m)
    { if (node.init) apply(*this, *node.init, m); }
    void ASTCodeGen::visit(FE::AST::ParamDeclarator& node, Module* m) { (void)node; (void)m; }

    void ASTCodeGen::visit(FE::AST::VarDeclaration& node, Module* m)
    {
        for (auto* d : *node.decls)
        {
            auto* l = dynamic_cast<FE::AST::LeftValExpr*>(d->lval);
            const auto& a = d->info;
            DataType t = convert(node.type);
            size_t ptr = getNewRegId();
            // alloca 放入口，避免循环每一轮重复分配栈空间。
            curFunc->getBlock(0)->insertFront(createAllocaInst(t, ptr, a.arrayDims));
            name2reg.addSymbol(l->entry, ptr);
            reg2attr[ptr] = a;
            paramPtrTab[ptr] = false;
            if (FE::AST::isTensor(node.type))
            {
                std::vector<Operand*> dims;
                for (int n : a.arrayDims) dims.push_back(getImmeI32Operand(n));
                tensorDims[ptr] = dims;
                if (auto* whole = dynamic_cast<FE::AST::Initializer*>(d->init))
                {
                    auto value = tensorEval(*whole->init_val, m);
                    tensorCopy(t, value, {getRegOperand(ptr), dims});
                    continue;
                }
            }
            if (!d->init) continue;
            if (a.arrayDims.empty())
            {
                auto* e = d->initElements[0];
                if (e)
                {
                    apply(*this, *e, m);
                    size_t value = castReg(convert(e->attr.val.value.type), t, getMaxReg());
                    insert(createStoreInst(t, value, getRegOperand(ptr)));
                }
                else insert(createStoreInst(t, t == DataType::F32 ? static_cast<Operand*>(getImmeF32Operand(0)) :
                    static_cast<Operand*>(getImmeI32Operand(0)), getRegOperand(ptr)));
                continue;
            }
            size_t count = d->initElements.size();
            insert(createCallInst(DataType::VOID, "llvm.memset.p0.i32", {
                {DataType::PTR, getRegOperand(ptr)}, {DataType::I8, getImmeI32Operand(0)},
                {DataType::I32, getImmeI32Operand(static_cast<int>(count * 4))}, {DataType::I1, getImmeI32Operand(0)}}));
            // 先转成首元素指针，再使用线性下标，对应语义阶段的行主序展平槽位。
            size_t base = getNewRegId();
            std::vector<Operand*> zeros(a.arrayDims.size() + 1, getImmeI32Operand(0));
            insert(createGEP_I32Inst(t, getRegOperand(ptr), a.arrayDims, zeros, base));
            for (size_t i = 0; i < count; ++i)
            {
                auto* e = d->initElements[i];
                if (!e) continue;
                apply(*this, *e, m);
                size_t value = castReg(convert(e->attr.val.value.type), t, getMaxReg());
                size_t dest = getNewRegId();
                insert(createGEP_I32Inst(t, getRegOperand(base), {}, {getImmeI32Operand(static_cast<int>(i))}, dest));
                insert(createStoreInst(t, value, getRegOperand(dest)));
            }
        }
    }
}
