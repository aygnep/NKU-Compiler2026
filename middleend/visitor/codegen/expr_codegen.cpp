#include <middleend/visitor/codegen/ast_codegen.h>

namespace ME
{
    Operand* ASTCodeGen::address(FE::AST::LeftValExpr& node, Module* m, bool decay)
    {
        size_t local = name2reg.getReg(node.entry);
        Operand* ptr = local == static_cast<size_t>(-1) ? static_cast<Operand*>(getGlobalOperand(node.entry->getName())) :
            static_cast<Operand*>(getRegOperand(local));
        auto dims = node.resolved.arrayDims;
        bool parameter = !dims.empty() && dims[0] == -1;
        if (dims.empty()) return ptr;
        if (parameter) dims.erase(dims.begin());
        std::vector<Operand*> indices;
        if (!parameter) indices.push_back(getImmeI32Operand(0));
        if (node.indices) for (auto* i : *node.indices)
        {
            apply(*this, *i, m);
            size_t index = castReg(convert(i->attr.val.value.type), DataType::I32, getMaxReg());
            indices.push_back(getRegOperand(index));
        }
        if (decay) indices.push_back(getImmeI32Operand(0));
        if (indices.empty()) return ptr;
        size_t result = getNewRegId();
        auto t = convert(FE::AST::TypeFactory::getBasicType(node.resolved.type->getBaseType()));
        insert(createGEP_I32Inst(t, ptr, dims, indices, result));
        return getRegOperand(result);
    }

    void ASTCodeGen::visit(FE::AST::LeftValExpr& node, Module* m)
    {
        bool decay = !node.remainingDims.empty();
        Operand* ptr = address(node, m, decay);
        if (decay)
        {
            // 数组形参无下标时仍生成一个结果寄存器，保持表达式返回值约定。
            if (ptr != getRegOperand(getMaxReg()))
            {
                size_t reg = getNewRegId();
                insert(createGEP_I32Inst(DataType::I32, ptr, {}, {getImmeI32Operand(0)}, reg));
            }
        }
        else { size_t reg = getNewRegId(); insert(createLoadInst(convert(node.attr.val.value.type), ptr, reg)); }
    }

    void ASTCodeGen::visit(FE::AST::LiteralExpr& node, Module* m)
    {
        (void)m;
        size_t reg = getNewRegId();
        if (convert(node.literal.type) == DataType::F32)
            insert(createArithmeticF32Inst_ImmeAll(Operator::FADD, node.literal.getFloat(), 0, reg));
        else insert(createArithmeticI32Inst_ImmeAll(Operator::ADD, node.literal.getInt(), 0, reg));
    }
    void ASTCodeGen::visit(FE::AST::UnaryExpr& node, Module* m)
    { handleUnaryCalc(*node.expr, node.op, curBlock, m); }

    void ASTCodeGen::handleAssign(FE::AST::LeftValExpr& lhs, FE::AST::ExprNode& rhs, Module* m)
    {
        Operand* ptr = address(lhs, m, false);
        apply(*this, rhs, m);
        auto t = convert(lhs.attr.val.value.type);
        size_t value = castReg(convert(rhs.attr.val.value.type), t, getMaxReg());
        insert(createStoreInst(t, value, ptr));
    }
    void ASTCodeGen::logical(FE::AST::BinaryExpr& node, Module* m, bool isAnd)
    {
        apply(*this, *node.lhs, m);
        size_t lhs = castReg(convert(node.lhs->attr.val.value.type), DataType::I1, getMaxReg());
        Block* left = curBlock;
        Block* right = createBlock();
        Block* end = createBlock();
        insert(createBranchInst(lhs, isAnd ? right->blockId : end->blockId, isAnd ? end->blockId : right->blockId));
        enterBlock(right);
        apply(*this, *node.rhs, m);
        size_t rhs = castReg(convert(node.rhs->attr.val.value.type), DataType::I1, getMaxReg());
        Block* rightExit = curBlock; // 右项本身也可能生成嵌套短路块
        jump(end->blockId);
        enterBlock(end);
        size_t result = getNewRegId();
        auto* phi = new PhiInst(DataType::I1, getRegOperand(result));
        phi->addIncoming(getImmeI32Operand(isAnd ? 0 : 1), getLabelOperand(left->blockId));
        phi->addIncoming(getRegOperand(rhs), getLabelOperand(rightExit->blockId));
        insert(phi);
    }
    void ASTCodeGen::handleLogicalAnd(FE::AST::BinaryExpr& node, FE::AST::ExprNode& lhs,
        FE::AST::ExprNode& rhs, Module* m) { (void)lhs; (void)rhs; logical(node, m, true); }
    void ASTCodeGen::handleLogicalOr(FE::AST::BinaryExpr& node, FE::AST::ExprNode& lhs,
        FE::AST::ExprNode& rhs, Module* m) { (void)lhs; (void)rhs; logical(node, m, false); }

    void ASTCodeGen::visit(FE::AST::BinaryExpr& node, Module* m)
    {
        using Op = FE::AST::Operator;
        if (node.op == Op::ASSIGN) handleAssign(*dynamic_cast<FE::AST::LeftValExpr*>(node.lhs), *node.rhs, m);
        else if (node.op == Op::AND) handleLogicalAnd(node, *node.lhs, *node.rhs, m);
        else if (node.op == Op::OR) handleLogicalOr(node, *node.lhs, *node.rhs, m);
        else handleBinaryCalc(*node.lhs, *node.rhs, node.op, curBlock, m);
    }
    void ASTCodeGen::visit(FE::AST::CallExpr& node, Module* m)
    {
        auto* f = funcDecls.at(node.func);
        CallInst::argList args;
        if (node.args) for (size_t i = 0; i < node.args->size(); ++i)
        {
            auto* a = (*node.args)[i];
            apply(*this, *a, m);
            DataType to = convert((*f->params)[i]->attr.val.value.type);
            size_t value = castReg(convert(a->attr.val.value.type), to, getMaxReg());
            args.push_back({to, getRegOperand(value)});
        }
        auto t = convert(f->retType);
        if (t == DataType::VOID) insert(createCallInst(t, node.func->getName(), args));
        else { size_t reg = getNewRegId(); insert(createCallInst(t, node.func->getName(), args, reg)); }
    }
    void ASTCodeGen::visit(FE::AST::CommaExpr& node, Module* m)
    { if (node.exprs) for (auto* e : *node.exprs) apply(*this, *e, m); }
}
