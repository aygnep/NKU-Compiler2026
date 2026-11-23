#include <frontend/symbol/symbol_table.h>
#include <debug.h>

namespace FE::Sym
{
    void SymTable::reset_impl() { scopes.assign(1, {}); }
    void SymTable::enterScope_impl() { scopes.emplace_back(); }
    void SymTable::exitScope_impl()
    {
        ASSERT(scopes.size() > 1);
        scopes.pop_back();
    }
    void SymTable::addSymbol_impl(Entry* entry, FE::AST::VarAttr& attr)
    {
        if (scopes.empty()) reset_impl();
        scopes.back()[entry] = attr;
    }
    FE::AST::VarAttr* SymTable::getSymbol_impl(Entry* entry)
    {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
        {
            auto found = it->find(entry);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }
    bool SymTable::isGlobalScope_impl() { return scopes.size() == 1; }
    int SymTable::getScopeDepth_impl() { return static_cast<int>(scopes.size()) - 1; }
}
