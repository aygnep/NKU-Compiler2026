#include <frontend/ast/visitor/sementic_check/ast_checker.h>
#include <cstdint>
#include <climits>
#include <functional>

namespace FE::AST
{
    void ASTChecker::inferTensorReturnShape(FuncDeclStmt& function)
    {
        // 在正式检查前读出固定返回形状，允许递归调用先于基本分支出现。
        std::map<Entry*, std::vector<int>> shapes;
        std::map<Entry*, long long> constants;
        std::function<bool(ExprNode*, long long&)> integer = [&](ExprNode* e, long long& value) {
            if (auto* l = dynamic_cast<LiteralExpr*>(e))
            { if (l->literal.type == floatType) return false; value=l->literal.getLL(); return true; }
            if (auto* l = dynamic_cast<LeftValExpr*>(e))
            {
                if (l->indices && !l->indices->empty()) return false;
                if (constants.count(l->entry)) { value=constants.at(l->entry); return true; }
                auto* a=symTable.getSymbol(l->entry);
                if (a && a->isConstDecl && a->arrayDims.empty() && !a->initList.empty())
                { value=a->initList[0].getLL(); return true; }
            }
            if (auto* u=dynamic_cast<UnaryExpr*>(e))
            { if (!integer(u->expr,value)) return false; if(u->op==Operator::SUB)value=-value; return u->op==Operator::ADD||u->op==Operator::SUB; }
            if (auto* b=dynamic_cast<BinaryExpr*>(e))
            {
                long long a=0,c=0; if(!integer(b->lhs,a)||!integer(b->rhs,c))return false;
                // 形状预读只在 int 范围内运算，完整表达式检查仍由正式访问负责。
                if(a<INT_MIN||a>INT_MAX||c<INT_MIN||c>INT_MAX)return false;
                switch(b->op)
                {
                    case Operator::ADD:value=a+c;return true;case Operator::SUB:value=a-c;return true;
                    case Operator::MUL:value=a*c;return true;case Operator::DIV:if(c){value=a/c;return true;}break;
                    case Operator::MOD:if(c){value=a%c;return true;}break;default:break;
                }
            }
            return false;
        };
        std::function<std::vector<int>(ExprNode*)> shape = [&](ExprNode* e)->std::vector<int> {
            if (auto* l=dynamic_cast<LeftValExpr*>(e))
            {
                if(l->indices&&!l->indices->empty())return {};
                if(shapes.count(l->entry))return shapes.at(l->entry);
                auto* a=symTable.getSymbol(l->entry);if(a&&isTensor(a->type))return a->arrayDims;
            }
            if(auto* u=dynamic_cast<UnaryExpr*>(e))return shape(u->expr);
            if(auto* b=dynamic_cast<BinaryExpr*>(e))
            {
                auto a=shape(b->lhs),c=shape(b->rhs);
                if(b->op==Operator::MATMUL)return a.size()==2&&c.size()==2?std::vector<int>{a[0],c[1]}:std::vector<int>{};
                return a.empty()?c:a;
            }
            if(auto* c=dynamic_cast<CallExpr*>(e))
            { auto it=funcDecls.find(c->func);if(it!=funcDecls.end())return it->second->returnShape; }
            if(auto* c=dynamic_cast<CommaExpr*>(e);c&&c->exprs&&!c->exprs->empty())return shape(c->exprs->back());
            return {};
        };
        std::function<void(StmtNode*)> walk = [&](StmtNode* stmt) {
            if(!stmt)return;
            if(auto* b=dynamic_cast<BlockStmt*>(stmt))
            { auto oldShapes=shapes;auto oldConstants=constants;if(b->stmts)for(auto* s:*b->stmts)walk(s);shapes=oldShapes;constants=oldConstants; }
            else if(auto* v=dynamic_cast<VarDeclStmt*>(stmt))
            {
                if(v->decl&&v->decl->decls)for(auto* d:*v->decl->decls)
                {
                    auto* l=dynamic_cast<LeftValExpr*>(d->lval);if(!l)continue;
                    shapes.erase(l->entry);constants.erase(l->entry);
                    if(isTensor(v->decl->type)&&l->indices)
                    {
                        std::vector<int> dims;bool known=true;
                        for(auto* e:*l->indices){long long n=0;if(!integer(e,n)||n<=0||n>INT_MAX){known=false;break;}dims.push_back(static_cast<int>(n));}
                        if(known)shapes[l->entry]=dims;
                    }
                    else if(v->decl->isConstDecl)
                    { auto* i=dynamic_cast<Initializer*>(d->init);long long n=0;if(i&&integer(i->init_val,n))constants[l->entry]=n; }
                }
            }
            else if(auto* ret=dynamic_cast<ReturnStmt*>(stmt))
            {
                auto dims=shape(ret->retExpr);bool fixed=!dims.empty();for(int n:dims)fixed&=n>0;
                if(fixed&&function.returnShape.empty())function.returnShape=dims;
            }
            else if(auto* i=dynamic_cast<IfStmt*>(stmt)){walk(i->thenStmt);walk(i->elseStmt);}
            else if(auto* w=dynamic_cast<WhileStmt*>(stmt))walk(w->body);
            else if(auto* f=dynamic_cast<ForStmt*>(stmt))walk(f->body);
        };
        walk(function.body);
    }

    bool ASTChecker::tensorSame(ExprNode& a, ExprNode& b, const Node& where)
    {
        if (a.attr.val.value.type != b.attr.val.value.type || a.tensorShape.size() != b.tensorShape.size())
            return fail(where, "tensor element type or rank mismatch");
        for (size_t i = 0; i < a.tensorShape.size(); ++i)
            if (a.tensorShape[i] > 0 && b.tensorShape[i] > 0 && a.tensorShape[i] != b.tensorShape[i])
                return fail(where, "tensor shape mismatch");
        return true; // 未知首维在 IR 中传递实际大小，并在运算/复制前检查。
    }

    bool ASTChecker::tensorBinary(BinaryExpr& node)
    {
        auto& a = *node.lhs; auto& b = *node.rhs;
        bool at = isTensor(a.attr.val.value.type), bt = isTensor(b.attr.val.value.type);
        if (node.op == Operator::ASSIGN)
        {
            auto* l = dynamic_cast<LeftValExpr*>(&a);
            if (!at || !bt || !l || !l->isLval)
                return fail(node, "tensor assignment requires writable tensor and tensor value");
            if (!tensorSame(a, b, node)) return false;
            node.attr.val.value.type = a.attr.val.value.type;
            node.tensorShape = a.tensorShape;
            return true;
        }
        bool matrix = node.op == Operator::MATMUL;
        if (matrix)
        {
            if (!at || !bt || a.tensorShape.size() != 2 || b.tensorShape.size() != 2)
                return fail(node, "matrix multiplication requires two rank-2 tensors");
            if (a.attr.val.value.type != b.attr.val.value.type)
                return fail(node, "matrix element type mismatch");
            if (a.tensorShape[1] > 0 && b.tensorShape[0] > 0 && a.tensorShape[1] != b.tensorShape[0])
                return fail(node, "matrix inner dimension mismatch");
            node.tensorShape = {a.tensorShape[0], b.tensorShape[1]};
        }
        else
        {
            if (node.op != Operator::ADD && node.op != Operator::SUB && node.op != Operator::MUL &&
                node.op != Operator::DIV && node.op != Operator::MOD)
                return fail(node, "comparison/logical operation does not accept tensor");
            if (at && bt && !tensorSame(a, b, node)) return false;
            auto* tensorType = at ? a.attr.val.value.type : b.attr.val.value.type;
            auto* base = TypeFactory::getBasicType(tensorType->getBaseType());
            if ((!at && a.attr.val.value.type != base) || (!bt && b.attr.val.value.type != base))
                return fail(node, "tensor and scalar element type mismatch");
            node.tensorShape = at ? a.tensorShape : b.tensorShape;
        }
        node.attr.val.value.type = at ? a.attr.val.value.type : b.attr.val.value.type;
        bool fp = node.attr.val.value.type->getBaseType() == Type_t::FLOAT;
        if (node.op == Operator::MOD && fp) return fail(node, "tensor modulus requires int elements");
        if (!fp && !bt && b.attr.val.isConstexpr && !b.attr.val.getInt() &&
            (node.op == Operator::DIV || node.op == Operator::MOD))
            return fail(node, "tensor operation with zero divisor");
        // 只为全局初值准备确定数据；运行期生成器始终读取当前内存，不折叠可变张量。
        bool knownA = at ? !a.tensorData.empty() : a.attr.val.isConstexpr;
        bool knownB = bt ? !b.tensorData.empty() : b.attr.val.isConstexpr;
        size_t count = 1;
        for (int n : node.tensorShape)
        {
            if (n <= 0) return true;
            if (count > static_cast<size_t>(INT_MAX / 4) / n) return fail(node, "tensor result is too large");
            count *= n;
        }
        if (!knownA || !knownB) return true;
        for (size_t i = 0; i < count; ++i)
        {
            if (matrix)
            {
                int rowsize = a.tensorShape[1], columns = b.tensorShape[1];
                VarValue sum = fp ? VarValue(0.0f) : VarValue(0);
                for (int k = 0; k < rowsize; ++k)
                {
                    const auto& x = a.tensorData[(i / columns) * rowsize + k];
                    const auto& y = b.tensorData[k * columns + (i % columns)];
                    if (fp) sum = VarValue(sum.getFloat() + x.getFloat() * y.getFloat());
                    else sum = VarValue(static_cast<int>(sum.getInt() + static_cast<int64_t>(x.getInt()) * y.getInt()));
                }
                node.tensorData.push_back(sum);
                continue;
            }
            auto x = at ? a.tensorData[i] : a.attr.val.value;
            auto y = bt ? b.tensorData[i] : b.attr.val.value;
            if (!fp && !y.getInt() && (node.op == Operator::DIV || node.op == Operator::MOD))
            {
                node.tensorData.clear();
                return currentFunction ? true : fail(node, "global tensor initializer has zero divisor");
            }
            if (fp)
            {
                float v = 0, l = x.getFloat(), r = y.getFloat();
                switch (node.op)
                {
                    case Operator::ADD: v=l+r; break; case Operator::SUB: v=l-r; break;
                    case Operator::MUL: v=l*r; break; case Operator::DIV: v=l/r; break;
                    default: break;
                }
                node.tensorData.emplace_back(v);
            }
            else
            {
                int64_t v = 0, l = x.getInt(), r = y.getInt();
                switch (node.op)
                {
                    case Operator::ADD: v=l+r; break; case Operator::SUB: v=l-r; break;
                    case Operator::MUL: v=l*r; break; case Operator::DIV: v=l/r; break;
                    case Operator::MOD: v=l%r; break; default: break;
                }
                node.tensorData.emplace_back(static_cast<int>(v));
            }
        }
        return true;
    }
}
