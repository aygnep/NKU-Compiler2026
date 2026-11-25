#include <frontend/ast/visitor/sementic_check/ast_checker.h>

namespace FE::AST
{
    bool ASTChecker::visit(LeftValExpr& node)
    {
        auto* found = symTable.getSymbol(node.entry);
        if (!found) return fail(node, "undeclared variable " + node.entry->getName());
        VarAttr a = *found;
        node.resolved = a;
        bool ok = true;
        size_t n = node.indices ? node.indices->size() : 0;
        if (n > a.arrayDims.size()) return fail(node, "too many array subscripts");
        if (node.indices) for (auto* i : *node.indices)
        {
            ok &= apply(*this, *i);
            auto* t = i->attr.val.value.type;
            if (t != intType && t != llType && t != boolType) ok &= fail(*i, "array index must be integer");
        }
        if (isTensor(a.type) && n && n != a.arrayDims.size())
            return fail(node, "tensor element access needs exactly rank subscripts");
        node.remainingDims.assign(a.arrayDims.begin() + n, a.arrayDims.end());
        node.isLval = node.remainingDims.empty() && !a.isConstDecl;
        node.attr.val.value.type = node.remainingDims.empty() ?
            TypeFactory::getBasicType(a.type->getBaseType()) : TypeFactory::getPtrType(TypeFactory::getBasicType(a.type->getBaseType()));
        if (isTensor(a.type) && n == 0)
        {
            node.isLval = !a.isConstDecl;
            node.tensorShape = a.arrayDims;
            node.attr.val.value.type = a.type;
            if (a.scopeLevel == 0 || a.isConstDecl) node.tensorData = a.initList;
        }
        node.attr.val.isConstexpr = false;
        if (a.isConstDecl && node.remainingDims.empty() && !a.initList.empty())
        {
            size_t offset = 0;
            bool constant = true;
            for (size_t j = 0; j < n; ++j)
            {
                auto& v = (*node.indices)[j]->attr.val;
                if (!v.isConstexpr) { constant = false; break; }
                int index = v.getInt();
                if (index < 0 || index >= a.arrayDims[j])
                { ok &= fail(node, "constant array index out of bounds"); constant = false; break; }
                offset = offset * a.arrayDims[j] + index;
            }
            if (constant) node.attr.val = ExprValue(a.initList[offset], true);
        }
        return ok;
    }
    bool ASTChecker::visit(LiteralExpr& node)
    { node.attr.val = ExprValue(node.literal, true); return true; }
    bool ASTChecker::visit(UnaryExpr& node)
    {
        if (!apply(*this, *node.expr)) return false;
        if (isTensor(node.expr->attr.val.value.type))
        {
            if (node.op != Operator::ADD && node.op != Operator::SUB)
                return fail(node, "logical operation does not accept tensor");
            node.attr.val.value.type = node.expr->attr.val.value.type;
            node.tensorShape = node.expr->tensorShape;
            node.tensorData = node.expr->tensorData;
            if (node.op == Operator::SUB) for (auto& v : node.tensorData)
                v = v.type == floatType ? VarValue(-v.getFloat()) : VarValue(static_cast<int>(-static_cast<long long>(v.getInt())));
            return true;
        }
        if (!scalar(*node.expr)) return false;
        bool error = false;
        node.attr.val = typeInfer(node.expr->attr.val, node.op, node, error);
        return !error;
    }
    bool ASTChecker::visit(BinaryExpr& node)
    {
        bool ok = apply(*this, *node.lhs);
        ok &= apply(*this, *node.rhs);
        if (!ok) return false;
        if (isTensor(node.lhs->attr.val.value.type) || isTensor(node.rhs->attr.val.value.type))
            return tensorBinary(node);
        if (node.op == Operator::MATMUL) return fail(node, "matrix multiplication requires two rank-2 tensors");
        if (!scalar(*node.lhs) || !scalar(*node.rhs)) return false;
        if (node.op == Operator::ASSIGN)
        {
            auto* l = dynamic_cast<LeftValExpr*>(node.lhs);
            if (!l || !l->isLval) return fail(node, "assignment needs a writable scalar lvalue");
            node.attr.val = ExprValue();
            node.attr.val.value.type = l->attr.val.value.type;
            return true;
        }
        if ((node.op == Operator::MOD || node.op == Operator::BITAND || node.op == Operator::BITOR) &&
            (node.lhs->attr.val.value.type == floatType || node.rhs->attr.val.value.type == floatType))
            return fail(node, "integer operands required for modulus/bitwise operation");
        bool error = false;
        node.attr.val = typeInfer(node.lhs->attr.val, node.rhs->attr.val, node.op, node, error);
        return !error;
    }
    bool ASTChecker::visit(CallExpr& node)
    {
        auto it = funcDecls.find(node.func);
        if (it == funcDecls.end() || symTable.getSymbol(node.func))
            return fail(node, "undeclared function or name is not callable: " + node.func->getName());
        auto* f = it->second;
        size_t nargs = node.args ? node.args->size() : 0;
        size_t nparams = f->params ? f->params->size() : 0;
        bool ok = true;
        if (node.args) for (auto* a : *node.args) ok &= apply(*this, *a);
        if (nargs != nparams) ok &= fail(node, "function argument count mismatch");
        if (!ok) return false;
        for (size_t i = 0; i < nargs; ++i)
        {
            auto* a = (*node.args)[i]; auto* p = (*f->params)[i];
            Type* target = p->attr.val.value.type;
            if (isTensor(target))
            {
                const auto& dims = p->info.arrayDims;
                if (a->attr.val.value.type != target || a->tensorShape.size() != dims.size())
                    ok &= fail(*a, "tensor argument type or rank mismatch");
                else for (size_t j = 1; j < dims.size(); ++j)
                    if (a->tensorShape[j] != dims[j]) ok &= fail(*a, "tensor argument shape mismatch");
                if (auto* l = dynamic_cast<LeftValExpr*>(a); l && l->resolved.isConstDecl)
                    ok &= fail(*a, "const tensor cannot bind writable tensor parameter");
            }
            else if (target->getTypeGroup() == TypeGroup::POINTER)
            {
                auto* l = dynamic_cast<LeftValExpr*>(a);
                if (!l || isTensor(a->attr.val.value.type) || l->remainingDims.empty() || a->attr.val.value.type->getBaseType() != target->getBaseType())
                { ok &= fail(*a, "array argument type mismatch"); continue; }
                const auto& dims = p->info.arrayDims;
                if (!dims.empty())
                {
                    if (dims.size() != l->remainingDims.size()) ok &= fail(*a, "array argument rank mismatch");
                    else for (size_t j = 1; j < dims.size(); ++j)
                        if (dims[j] != l->remainingDims[j]) ok &= fail(*a, "array argument dimension mismatch");
                }
            }
            else ok &= scalar(*a);
        }
        if (isTensor(f->retType))
        {
            if (f->returnShape.empty()) return fail(node, "tensor return shape must be known before call");
            node.tensorShape = f->returnShape;
        }
        node.attr.val.value.type = f->retType;
        node.attr.val.isConstexpr = false;
        return ok;
    }
    bool ASTChecker::visit(CommaExpr& node)
    {
        bool ok = true;
        if (node.exprs) for (auto* e : *node.exprs) { ok &= apply(*this, *e); node.attr = e->attr; node.tensorShape = e->tensorShape; }
        // 即使末项为字面量，前面的函数调用/赋值也不能被常量折叠跳过。
        node.attr.val.isConstexpr = false;
        return ok;
    }
}
