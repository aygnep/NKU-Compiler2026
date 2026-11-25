#include <middleend/visitor/codegen/ast_codegen.h>

namespace ME
{
    void ASTCodeGen::visit(FE::AST::ExprStmt& node, Module* m)
    { if (node.expr) apply(*this, *node.expr, m); }
    void ASTCodeGen::visit(FE::AST::FuncDeclStmt& node, Module* m)
    {
        bool returnsTensor = FE::AST::isTensor(node.retType);
        auto* def = new FuncDefInst(returnsTensor ? DataType::VOID : convert(node.retType), node.entry->getName());
        auto* func = new Function(def);
        m->functions.push_back(func);
        enterFunc(func);
        enterBlock(createBlock());
        name2reg.enterScope();
        reg2attr.clear(); paramPtrTab.clear(); loops.clear(); tensorDims.clear(); tensorValues.clear();
        tensorReturn = nullptr;
        if (returnsTensor)
        {
            tensorReturn = getRegOperand(getNewRegId());
            def->argRegs.push_back({DataType::PTR, tensorReturn});
        }
        std::map<FE::AST::ParamDeclarator*, size_t> parameterRegs;
        std::map<FE::AST::ParamDeclarator*, size_t> extentRegs;
        if (node.params)
        {
            // 所有参数寄存器先分配，再建立标量参数的栈槽。
            for (auto* p : *node.params)
            {
                size_t reg = getNewRegId();
                bool tensor = FE::AST::isTensor(p->type);
                parameterRegs[p] = reg;
                def->argRegs.push_back({tensor ? DataType::PTR : convert(p->attr.val.value.type), getRegOperand(reg)});
                if (tensor)
                {
                    size_t extent = getNewRegId();
                    extentRegs[p] = extent;
                    def->argRegs.push_back({DataType::I32, getRegOperand(extent)});
                }
            }
            for (size_t i = 0; i < node.params->size(); ++i)
            {
                auto* p = (*node.params)[i];
                size_t reg = parameterRegs.at(p);
                if (FE::AST::isTensor(p->type))
                {
                    std::vector<Operand*> dims{getRegOperand(extentRegs.at(p))};
                    for (size_t j = 1; j < p->info.arrayDims.size(); ++j)
                        dims.push_back(getImmeI32Operand(p->info.arrayDims[j]));
                    tensorDims[reg] = dims;
                }
                bool pointer = !p->info.arrayDims.empty();
                if (!pointer)
                {
                    size_t slot = getNewRegId();
                    insert(createAllocaInst(convert(p->type), slot));
                    insert(createStoreInst(convert(p->type), reg, getRegOperand(slot)));
                    reg = slot;
                }
                name2reg.addSymbol(p->entry, reg);
                reg2attr[reg] = p->info;
                paramPtrTab[reg] = pointer;
            }
        }
        allocationBlock = curBlock;
        Block* bodyBlock = createBlock();
        jump(bodyBlock->blockId);
        enterBlock(bodyBlock);
        if (node.body) apply(*this, *node.body, m);
        // 不可达的合流块仍需合法终结；非 void 落到函数末尾按课程约定补零。
        for (auto& [id, block] : func->blocks)
        {
            (void)id;
            if (!block->insts.empty() && block->insts.back()->isTerminator()) continue;
            if (returnsTensor || node.retType == FE::AST::voidType) block->insertBack(createRetInst());
            else if (node.retType == FE::AST::floatType) block->insertBack(createRetInst(0.0f));
            else block->insertBack(createRetInst(0));
        }
        name2reg.exitScope();
        exitBlock(); exitFunc();
    }
    void ASTCodeGen::visit(FE::AST::VarDeclStmt& node, Module* m)
    { if (node.decl) apply(*this, *node.decl, m); }
    void ASTCodeGen::visit(FE::AST::BlockStmt& node, Module* m)
    {
        name2reg.enterScope();
        if (node.stmts) for (auto* s : *node.stmts)
        {
            if (terminated()) break;
            apply(*this, *s, m);
        }
        name2reg.exitScope();
    }
    void ASTCodeGen::visit(FE::AST::ReturnStmt& node, Module* m)
    {
        if (!node.retExpr) { insert(createRetInst()); return; }
        if (tensorReturn)
        {
            auto value = tensorEval(*node.retExpr, m);
            tensorCopy(convert(node.retExpr->attr.val.value.type), value, {tensorReturn, value.dims});
            insert(createRetInst());
            return;
        }
        apply(*this, *node.retExpr, m);
        auto target = curFunc->funcDef->retType;
        size_t result = castReg(convert(node.retExpr->attr.val.value.type), target, getMaxReg());
        insert(createRetInst(target, result));
    }
    void ASTCodeGen::visit(FE::AST::IfStmt& node, Module* m)
    {
        auto* yes = createBlock(); auto* no = createBlock(); auto* end = createBlock();
        apply(*this, *node.cond, m);
        size_t cond = castReg(convert(node.cond->attr.val.value.type), DataType::I1, getMaxReg());
        insert(createBranchInst(cond, yes->blockId, no->blockId));
        enterBlock(yes);
        if (node.thenStmt) apply(*this, *node.thenStmt, m);
        jump(end->blockId);
        enterBlock(no);
        if (node.elseStmt) apply(*this, *node.elseStmt, m);
        jump(end->blockId);
        enterBlock(end);
    }
    void ASTCodeGen::visit(FE::AST::WhileStmt& node, Module* m)
    {
        auto* condBlock = createBlock(); auto* body = createBlock(); auto* end = createBlock();
        jump(condBlock->blockId);
        enterBlock(condBlock);
        apply(*this, *node.cond, m);
        size_t cond = castReg(convert(node.cond->attr.val.value.type), DataType::I1, getMaxReg());
        insert(createBranchInst(cond, body->blockId, end->blockId));
        loops.push_back({condBlock->blockId, end->blockId});
        enterBlock(body);
        if (node.body) apply(*this, *node.body, m);
        jump(condBlock->blockId);
        loops.pop_back();
        enterBlock(end);
    }
    void ASTCodeGen::visit(FE::AST::BreakStmt& node, Module* m)
    { (void)node; (void)m; jump(loops.back().second); }
    void ASTCodeGen::visit(FE::AST::ContinueStmt& node, Module* m)
    { (void)node; (void)m; jump(loops.back().first); }
    void ASTCodeGen::visit(FE::AST::ForStmt& node, Module* m)
    {
        name2reg.enterScope();
        if (node.init) apply(*this, *node.init, m);
        auto* condBlock = createBlock(); auto* body = createBlock();
        auto* step = createBlock(); auto* end = createBlock();
        jump(condBlock->blockId);
        enterBlock(condBlock);
        if (node.cond)
        {
            apply(*this, *node.cond, m);
            size_t cond = castReg(convert(node.cond->attr.val.value.type), DataType::I1, getMaxReg());
            insert(createBranchInst(cond, body->blockId, end->blockId));
        }
        else jump(body->blockId);
        loops.push_back({step->blockId, end->blockId});
        enterBlock(body);
        if (node.body) apply(*this, *node.body, m);
        jump(step->blockId);
        enterBlock(step);
        if (node.step) apply(*this, *node.step, m);
        jump(condBlock->blockId);
        loops.pop_back();
        enterBlock(end);
        name2reg.exitScope();
    }
}
