#include <frontend/ast/visitor/sementic_check/ast_checker.h>
#include <limits>

namespace FE::AST
{
    bool ASTChecker::scalar(ExprNode& node)
    {
        auto* t = node.attr.val.value.type;
        return (t != voidType && t->getTypeGroup() == TypeGroup::BASIC) ||
            fail(node, "expected numeric scalar (void/array is not a numeric value)");
    }

    bool ASTChecker::dimensions(std::vector<ExprNode*>* dims, std::vector<int>& out, bool parameter)
    {
        bool ok = true;
        if (!dims) return true;
        size_t total = 1;
        for (size_t i = 0; i < dims->size(); ++i)
        {
            auto* d = (*dims)[i];
            if (!d && parameter && i == 0) { out.push_back(-1); continue; }
            if (!d) { ok = false; continue; }
            bool valid = apply(*this, *d);
            auto& v = d->attr.val;
            // 语法阶段以字面量 -1 表示形参首维的 []，不是真实负维度。
            if (parameter && i == 0 && valid && v.isConstexpr && v.value.type == intType && v.getInt() == -1)
            { out.push_back(-1); continue; }
            valid &= v.isConstexpr && (v.value.type == intType || v.value.type == llType);
            if (!valid || v.getLL() <= 0 || v.getLL() > std::numeric_limits<int>::max())
            { ok &= fail(*d, "array dimension must be a positive integer constant"); out.push_back(1); }
            else
            {
                out.push_back(v.getInt());
                if (total > static_cast<size_t>(std::numeric_limits<int>::max() / 4) / out.back())
                    ok &= fail(*d, "array object is too large");
                else total *= out.back();
            }
        }
        return ok;
    }

    // 根据当前位置可容纳的最大对齐子数组递归展平；容量始终由父初始化器限定。
    bool ASTChecker::flatten(InitDecl* init, const std::vector<int>& dims, size_t depth,
        size_t start, size_t capacity, std::vector<ExprNode*>& out)
    {
        if (auto* one = dynamic_cast<Initializer*>(init))
        { out[start] = one->init_val; return true; }
        auto* list = dynamic_cast<InitializerList*>(init);
        if (!list || !list->init_list) return true;
        size_t cursor = 0;
        bool ok = true;
        for (auto* child : *list->init_list)
        {
            if (cursor >= capacity) { ok &= fail(*child, "excess elements in initializer"); continue; }
            size_t span = 1, next = dims.size();
            if (dynamic_cast<InitializerList*>(child))
            {
                for (size_t d = depth + 1; d < dims.size(); ++d)
                {
                    size_t candidate = 1;
                    for (size_t j = d; j < dims.size(); ++j) candidate *= dims[j];
                    if (cursor % candidate == 0 && candidate <= capacity - cursor)
                    { span = candidate; next = d; break; }
                }
            }
            ok &= flatten(child, dims, next, start + cursor, span, out);
            cursor += span;
        }
        return ok;
    }

    bool ASTChecker::visit(Initializer& node)
    {
        bool ok = apply(*this, *node.init_val);
        node.attr = node.init_val->attr;
        return ok && scalar(*node.init_val);
    }
    bool ASTChecker::visit(InitializerList& node)
    {
        bool ok = true;
        if (node.init_list) for (auto* i : *node.init_list) ok &= apply(*this, *i);
        return ok;
    }
    bool ASTChecker::visit(VarDeclarator& node)
    { return !node.init || apply(*this, *node.init); }

    bool ASTChecker::visit(ParamDeclarator& node)
    {
        bool ok = true;
        VarAttr a(node.type, false, symTable.getScopeDepth());
        ok &= dimensions(node.dims, a.arrayDims, true);
        if (node.type == voidType) ok &= fail(node, "void parameter");
        if (isTensor(node.type))
        {
            if (a.arrayDims.empty() || a.arrayDims[0] != -1)
                ok &= fail(node, "tensor parameter must start with []");
        }
        else if (!a.arrayDims.empty()) a.type = TypeFactory::getPtrType(node.type);
        auto* old = symTable.getSymbol(node.entry);
        if (old && old->scopeLevel == a.scopeLevel) ok &= fail(node, "duplicate parameter " + node.entry->getName());
        node.info = a;
        node.attr.val.value.type = a.type;
        symTable.addSymbol(node.entry, a);
        return ok;
    }

    bool ASTChecker::visit(VarDeclaration& node)
    {
        bool ok = true;
        if (node.type == voidType) return fail(node, "void variable");
        for (auto* d : *node.decls)
        {
            auto* l = dynamic_cast<LeftValExpr*>(d->lval);
            if (!l) { ok &= fail(*d, "invalid declarator"); continue; }
            VarAttr a(node.type, node.isConstDecl, symTable.getScopeDepth());
            bool valid = dimensions(l->indices, a.arrayDims, false);
            auto* old = symTable.getSymbol(l->entry);
            if ((old && old->scopeLevel == a.scopeLevel) ||
                (symTable.isGlobalScope() && funcDecls.count(l->entry)))
                valid &= fail(*d, "duplicate declaration " + l->entry->getName());
            if (node.isConstDecl && !d->init) valid &= fail(*d, "const declaration needs initializer");
            if (isTensor(node.type) && a.arrayDims.empty())
                valid &= fail(*d, "tensor declaration requires positive dimensions");
            // 先加入当前声明，初始化表达式中的名字按声明点规则解析。
            symTable.addSymbol(l->entry, a);
            // 张量整体表达式初值与逐元素初值列表走不同路径。
            auto* whole = isTensor(node.type) ? dynamic_cast<Initializer*>(d->init) : nullptr;
            if (whole)
            {
                valid &= apply(*this, *whole->init_val);
                auto* e = whole->init_val;
                if (!isTensor(e->attr.val.value.type) || e->attr.val.value.type != node.type ||
                    e->tensorShape.size() != a.arrayDims.size())
                    valid &= fail(*d, "tensor initializer type or shape mismatch");
                else for (size_t i = 0; i < a.arrayDims.size(); ++i)
                    if (e->tensorShape[i] > 0 && e->tensorShape[i] != a.arrayDims[i])
                        valid &= fail(*d, "tensor initializer shape mismatch");
                if (symTable.isGlobalScope() || node.isConstDecl)
                {
                    if (e->tensorData.empty()) valid &= fail(*d, "global/const tensor initializer must be compile-time evaluable");
                    else a.initList = e->tensorData;
                }
                d->info = a; l->resolved = a; l->tensorShape = a.arrayDims;
                l->attr.val.value.type = node.type;
                symTable.addSymbol(l->entry, a);
                if (symTable.isGlobalScope()) glbSymbols[l->entry] = a;
                ok &= valid;
                continue;
            }
            if (d->init) valid &= apply(*this, *d->init);
            if (!valid) { ok = false; continue; }
            size_t count = 1;
            for (int n : a.arrayDims) count *= n;
            if (d->init || (symTable.isGlobalScope() && a.arrayDims.empty()) || node.isConstDecl)
            {
                d->initElements.assign(count, nullptr);
                if (d->init) valid &= flatten(d->init, a.arrayDims, 0, 0, count, d->initElements);
                if (!valid) { ok = false; continue; }
                a.initList.reserve(count);
                for (auto* e : d->initElements)
                {
                    if (e && (node.isConstDecl || symTable.isGlobalScope()) && !e->attr.val.isConstexpr)
                        valid &= fail(*e, "initializer must be a constant expression");
                    auto* base = TypeFactory::getBasicType(node.type->getBaseType());
                    if (isTensor(node.type) && e && e->attr.val.value.type != base)
                        valid &= fail(*e, "tensor initializer element type mismatch");
                    VarValue v = base == floatType ? VarValue(0.0f) : VarValue(0);
                    if (e && e->attr.val.isConstexpr)
                        v = node.type->getBaseType() == Type_t::FLOAT ? VarValue(e->attr.val.getFloat()) : VarValue(e->attr.val.getInt());
                    a.initList.push_back(v);
                }
            }
            d->info = a;
            d->attr.val.value.type = node.type;
            l->resolved = a;
            l->attr.val.value.type = node.type;
            symTable.addSymbol(l->entry, a);
            if (symTable.isGlobalScope()) glbSymbols[l->entry] = a;
            ok &= valid;
        }
        return ok;
    }
}
