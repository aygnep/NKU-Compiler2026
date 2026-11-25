#include <middleend/visitor/codegen/ast_codegen.h>

namespace ME
{
    ASTCodeGen::TensorValue ASTCodeGen::tensorEval(FE::AST::ExprNode& node, Module* m)
    {
        apply(*this, node, m);
        return tensorValues.at(&node);
    }

    Operand* ASTCodeGen::scalarIR(Operator op, DataType t, Operand* a, Operand* b)
    {
        if (t == DataType::F32)
        {
            if (op == Operator::ADD) op = Operator::FADD;
            else if (op == Operator::SUB) op = Operator::FSUB;
            else if (op == Operator::MUL) op = Operator::FMUL;
            else if (op == Operator::DIV) op = Operator::FDIV;
        }
        auto* result = getRegOperand(getNewRegId());
        insert(new ArithmeticInst(op, t, a, b, result));
        return result;
    }

    Operand* ASTCodeGen::tensorCount(const std::vector<Operand*>& dims)
    {
        Operand* count = getImmeI32Operand(1);
        for (auto* d : dims)
        {
            auto* a = dynamic_cast<ImmeI32Operand*>(count);
            auto* b = dynamic_cast<ImmeI32Operand*>(d);
            if (a && b) count = getImmeI32Operand(a->value * b->value);
            else
            {
                auto* limit = scalarIR(Operator::DIV, DataType::I32, getImmeI32Operand(536870911), d);
                auto* fits = getRegOperand(getNewRegId());
                insert(new IcmpInst(DataType::I32, ICmpOp::SLE, count, limit, fits));
                tensorCheck(fits, getImmeI32Operand(1), DataType::I1);
                count = scalarIR(Operator::MUL, DataType::I32, count, d);
            }
        }
        return count;
    }

    ASTCodeGen::TensorValue ASTCodeGen::tensorTemp(DataType t, const std::vector<Operand*>& dims)
    {
        // dims 只由本函数的形参首维或常量组成，均在函数序言可用。
        // 保存序言到函数体的边，追加空间计算后恢复，避免循环每轮 alloca。
        Block* saved = curBlock;
        enterBlock(allocationBlock);
        Instruction* exit = curBlock->insts.back();
        curBlock->insts.pop_back();
        Operand* count = tensorCount(dims);
        auto* result = getRegOperand(getNewRegId());
        auto* allocation = new AllocaInst(t, result);
        allocation->dynamicCount = count;
        insert(allocation);
        insert(exit);
        allocationBlock = curBlock;
        enterBlock(saved);
        return {result, dims};
    }

    Operand* ASTCodeGen::elementPtr(DataType t, Operand* ptr, Operand* index)
    {
        size_t reg = getNewRegId();
        insert(createGEP_I32Inst(t, ptr, {}, {index}, reg));
        return getRegOperand(reg);
    }

    void ASTCodeGen::tensorLoop(Operand* count, const std::function<void(Operand*)>& body)
    {
        Block* pre = curBlock;
        Block* condition = createBlock(); Block* work = createBlock(); Block* end = createBlock();
        jump(condition->blockId);
        enterBlock(condition);
        auto* index = getRegOperand(getNewRegId());
        auto* phi = new PhiInst(DataType::I32, index);
        phi->addIncoming(getImmeI32Operand(0), getLabelOperand(pre->blockId));
        insert(phi);
        auto* test = getRegOperand(getNewRegId());
        insert(new IcmpInst(DataType::I32, ICmpOp::SLT, index, count, test));
        insert(createBranchInst(test->getRegNum(), work->blockId, end->blockId));
        enterBlock(work);
        body(index);
        auto* next = scalarIR(Operator::ADD, DataType::I32, index, getImmeI32Operand(1));
        phi->addIncoming(next, getLabelOperand(curBlock->blockId));
        jump(condition->blockId);
        enterBlock(end);
    }

    void ASTCodeGen::tensorCheck(Operand* a, Operand* b, DataType type)
    {
        if (a == b) return;
        auto* test = getRegOperand(getNewRegId());
        insert(new IcmpInst(type, ICmpOp::EQ, a, b, test));
        Block* good = createBlock(); Block* bad = createBlock();
        insert(createBranchInst(test->getRegNum(), good->blockId, bad->blockId));
        enterBlock(bad);
        insert(createCallInst(DataType::VOID, "llvm.trap"));
        auto t = curFunc->funcDef->retType;
        if (t == DataType::VOID) insert(createRetInst());
        else if (t == DataType::F32) insert(createRetInst(0.0f));
        else insert(createRetInst(0));
        enterBlock(good);
    }

    void ASTCodeGen::tensorCopy(DataType t, const TensorValue& from, const TensorValue& to)
    {
        for (size_t i = 0; i < to.dims.size(); ++i) tensorCheck(from.dims[i], to.dims[i]);
        tensorLoop(tensorCount(to.dims), [&](Operand* i) {
            auto* ptr = elementPtr(t, from.ptr, i);
            size_t reg = getNewRegId(); insert(createLoadInst(t, ptr, reg));
            auto* dest = elementPtr(t, to.ptr, i);
            insert(createStoreInst(t, getRegOperand(reg), dest));
        });
    }

    void ASTCodeGen::tensorUnary(FE::AST::UnaryExpr& node, Module* m)
    {
        auto from = tensorEval(*node.expr, m);
        auto t = convert(node.attr.val.value.type);
        auto to = tensorTemp(t, from.dims);
        tensorLoop(tensorCount(from.dims), [&](Operand* i) {
            size_t reg = getNewRegId(); insert(createLoadInst(t, elementPtr(t, from.ptr, i), reg));
            Operand* value = getRegOperand(reg);
            if (node.op == FE::AST::Operator::SUB)
            {
                Operand* zero = t == DataType::F32 ? static_cast<Operand*>(getImmeF32Operand(-0.0f)) :
                    static_cast<Operand*>(getImmeI32Operand(0));
                value = scalarIR(Operator::SUB, t, zero, value);
            }
            insert(createStoreInst(t, value, elementPtr(t, to.ptr, i)));
        });
        tensorValues[&node] = to;
    }

    void ASTCodeGen::tensorBinary(FE::AST::BinaryExpr& node, Module* m)
    {
        using Op = FE::AST::Operator;
        bool at = FE::AST::isTensor(node.lhs->attr.val.value.type);
        bool bt = FE::AST::isTensor(node.rhs->attr.val.value.type);
        auto t = convert(node.attr.val.value.type);
        TensorValue a{}, b{}; Operand* av = nullptr; Operand* bv = nullptr;
        if (at) a = tensorEval(*node.lhs, m);
        else { apply(*this, *node.lhs, m); av = getRegOperand(getMaxReg()); }
        if (bt) b = tensorEval(*node.rhs, m);
        else { apply(*this, *node.rhs, m); bv = getRegOperand(getMaxReg()); }
        if (node.op == Op::ASSIGN)
        {
            // 先拍下右值，防止 a=a@a 或别名参数复制覆盖尚未读完的源。
            auto snapshot = tensorTemp(t, b.dims);
            tensorCopy(t, b, snapshot);
            tensorCopy(t, snapshot, a);
            tensorValues[&node] = a;
            return;
        }
        if (node.op == Op::MATMUL)
        {
            tensorCheck(a.dims[1], b.dims[0]);
            auto to = tensorTemp(t, {a.dims[0], b.dims[1]});
            auto acc = tensorTemp(t, {getImmeI32Operand(1)});
            tensorLoop(tensorCount(to.dims), [&](Operand* i) {
                Operand* zero = t == DataType::F32 ? static_cast<Operand*>(getImmeF32Operand(0)) :
                    static_cast<Operand*>(getImmeI32Operand(0));
                insert(createStoreInst(t, zero, acc.ptr));
                auto* row = scalarIR(Operator::DIV, DataType::I32, i, b.dims[1]);
                auto* col = scalarIR(Operator::MOD, DataType::I32, i, b.dims[1]);
                tensorLoop(a.dims[1], [&](Operand* k) {
                    auto* ai = scalarIR(Operator::ADD, DataType::I32,
                        scalarIR(Operator::MUL, DataType::I32, row, a.dims[1]), k);
                    auto* bi = scalarIR(Operator::ADD, DataType::I32,
                        scalarIR(Operator::MUL, DataType::I32, k, b.dims[1]), col);
                    auto* ap = elementPtr(t, a.ptr, ai); auto* bp = elementPtr(t, b.ptr, bi);
                    size_t x = getNewRegId(); insert(createLoadInst(t, ap, x));
                    size_t y = getNewRegId(); insert(createLoadInst(t, bp, y));
                    auto* product = scalarIR(Operator::MUL, t, getRegOperand(x), getRegOperand(y));
                    size_t old = getNewRegId(); insert(createLoadInst(t, acc.ptr, old));
                    auto* sum = scalarIR(Operator::ADD, t, getRegOperand(old), product);
                    insert(createStoreInst(t, sum, acc.ptr));
                });
                size_t reg = getNewRegId(); insert(createLoadInst(t, acc.ptr, reg));
                insert(createStoreInst(t, getRegOperand(reg), elementPtr(t, to.ptr, i)));
            });
            tensorValues[&node] = to;
            return;
        }
        auto dims = at ? a.dims : b.dims;
        if (at && bt) for (size_t i = 0; i < dims.size(); ++i) tensorCheck(a.dims[i], b.dims[i]);
        auto to = tensorTemp(t, dims);
        Operator op = Operator::ADD;
        switch (node.op)
        {
            case Op::ADD: op=Operator::ADD; break; case Op::SUB: op=Operator::SUB; break;
            case Op::MUL: op=Operator::MUL; break; case Op::DIV: op=Operator::DIV; break;
            case Op::MOD: op=Operator::MOD; break; default: ERROR("invalid checked tensor operation");
        }
        tensorLoop(tensorCount(dims), [&](Operand* i) {
            Operand* x = av; Operand* y = bv;
            if (at) { auto* p = elementPtr(t, a.ptr, i); size_t reg = getNewRegId(); insert(createLoadInst(t, p, reg)); x=getRegOperand(reg); }
            if (bt) { auto* p = elementPtr(t, b.ptr, i); size_t reg = getNewRegId(); insert(createLoadInst(t, p, reg)); y=getRegOperand(reg); }
            auto* value = scalarIR(op, t, x, y);
            insert(createStoreInst(t, value, elementPtr(t, to.ptr, i)));
        });
        tensorValues[&node] = to;
    }
}
