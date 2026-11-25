#include <frontend/ast/visitor/sementic_check/ast_checker.h>

namespace FE::AST
{
    bool ASTChecker::visit(ExprStmt& node) { return !node.expr || apply(*this, *node.expr); }
    bool ASTChecker::visit(FuncDeclStmt& node)
    {
        if (!symTable.isGlobalScope()) return fail(node, "nested function definition");
        if (funcDecls.count(node.entry) || symTable.getSymbol(node.entry))
            return fail(node, "duplicate function " + node.entry->getName());
        funcDecls[node.entry] = &node; // 先登记再检查函数体，允许递归
        if (node.entry->getName() == "main" && node.body) mainExists = true;
        currentFunction = &node;
        if (node.entry->getName() == "main" && isTensor(node.retType)) return fail(node, "main must return int");
        curFuncRetType = node.retType;
        funcHasReturn = false;
        symTable.enterScope();
        bool ok = true;
        if (node.params) for (auto* p : *node.params) ok &= apply(*this, *p);
        if (isTensor(node.retType)) inferTensorReturnShape(node);
        if (node.body) ok &= apply(*this, *node.body);
        symTable.exitScope();
        if (isTensor(node.retType) && node.returnShape.empty()) ok &= fail(node, "tensor function needs fixed-shape return");
        currentFunction = nullptr;
        curFuncRetType = voidType;
        return ok;
    }
    bool ASTChecker::visit(VarDeclStmt& node) { return !node.decl || apply(*this, *node.decl); }
    bool ASTChecker::visit(BlockStmt& node)
    {
        symTable.enterScope();
        bool ok = true;
        if (node.stmts) for (auto* s : *node.stmts) ok &= apply(*this, *s);
        symTable.exitScope();
        return ok;
    }
    bool ASTChecker::visit(ReturnStmt& node)
    {
        funcHasReturn = true;
        if (!node.retExpr)
            return curFuncRetType == voidType || fail(node, "non-void function needs return value");
        bool ok = apply(*this, *node.retExpr);
        if (curFuncRetType == voidType) ok &= fail(node, "void function cannot return a value");
        else if (isTensor(curFuncRetType))
        {
            auto& shape = node.retExpr->tensorShape;
            if (node.retExpr->attr.val.value.type != curFuncRetType || shape.empty())
                ok &= fail(node, "tensor return type mismatch");
            else
            {
                for (int n : shape) if (n <= 0) ok &= fail(node, "returned tensor shape must be compile-time known");
                if (currentFunction->returnShape.empty()) currentFunction->returnShape = shape;
                else if (currentFunction->returnShape != shape) ok &= fail(node, "tensor return shape mismatch");
            }
        }
        else ok &= scalar(*node.retExpr);
        return ok;
    }
    bool ASTChecker::visit(WhileStmt& node)
    {
        bool ok = apply(*this, *node.cond);
        ok &= scalar(*node.cond);
        ++loopDepth;
        if (node.body) ok &= apply(*this, *node.body);
        --loopDepth;
        return ok;
    }
    bool ASTChecker::visit(IfStmt& node)
    {
        bool ok = apply(*this, *node.cond);
        ok &= scalar(*node.cond);
        if (node.thenStmt) ok &= apply(*this, *node.thenStmt);
        if (node.elseStmt) ok &= apply(*this, *node.elseStmt);
        return ok;
    }
    bool ASTChecker::visit(BreakStmt& node)
    { return loopDepth != 0 || fail(node, "break outside loop"); }
    bool ASTChecker::visit(ContinueStmt& node)
    { return loopDepth != 0 || fail(node, "continue outside loop"); }
    bool ASTChecker::visit(ForStmt& node)
    {
        symTable.enterScope();
        bool ok = !node.init || apply(*this, *node.init);
        if (node.cond) { ok &= apply(*this, *node.cond); ok &= scalar(*node.cond); }
        if (node.step) ok &= apply(*this, *node.step);
        ++loopDepth;
        if (node.body) ok &= apply(*this, *node.body);
        --loopDepth;
        symTable.exitScope();
        return ok;
    }
}
