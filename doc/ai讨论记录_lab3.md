小组成员姓名：彭亚滨

学号：2413460

GitLab 仓库：[compiler-principles/lexer](https://gitlab.eduxiji.net/compiler-principles/lexer)

# 实验3 张量扩展与 LLVM IR 实现

> 新版手册多了 tensor 和 @，原来 Lab3 已经两组 100/100，这次从哪里继续？先不要 push。

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `frontend/ast/visitor/sementic_check/ast_checker.h`
> - Read `middleend/visitor/codegen/ast_codegen.h`
>
> </details>

</details>

原官方输入没有 tensor/@，旧测试通过只能说明原范围。本次从已有 Lab3-1 的语义访问者和 Lab3-2 的 IR 访问者继续扩展。新版第 9–12 页规定张量语义，第 25 页把张量列入进阶；for 也列为基础，但原实现已经支持。11 月 9 日纪要没有张量内容，新增部分按新版 PDF 和《张量实现要求.pdf》执行。

> 先把输入到输出的过程串一下。我需要重新写一套编译器吗？

沿用现有流程：

```text
.sy → lexer.l 的 tensor/@ token
    → yacc.y 的 TensorType / MATMUL 节点
    → ASTChecker 的元素类型、秩与形状检查
    → ASTCodeGen 的张量缓冲与循环 IR
    → llvm-as → 链接运行 → stdout/返回值
```

AST 和 IR 的访问者接口继续使用。新增 TensorType 区分普通数组与整体张量，TensorValue 则为代码生成保存指针和实际尺寸。错误输入在语义阶段退出，未知首维的运行期不匹配另做检查。

> 比如这个，为什么 b*2 不是矩阵乘？
>
> ```c
> tensor int a[4]={1,2,3,4};
> tensor int b[4]={5,6,7,8};
> tensor int c[4]=a+b*2+10;
> ```

* 明确是逐元素乘，@ 才是矩阵乘。b*2 把标量 2 提升成同形状值，得到 [10,12,14,16]；加 a 后为 [11,14,17,20]；再加 10 得 [21,24,27,30]。

IR 不必真的分配一个全是 2 的数组。先求出标量寄存器，再在元素循环中复用它，副作用函数也只能求一次。

> 普通 int a[4] 也有形状，直接让数组支持加法不行吗？

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `interfaces/frontend/ast/ast_defs.h`
> - Read `frontend/ast/expr.h`
> - Read `frontend/ast/visitor/sementic_check/expr_checker.cpp`
>
> </details>

</details>

需要保留类型边界。TensorType 保存 int/float 基类型，ExprNode::tensorShape 保存维度。普通数组继续按原来的下标与指针规则处理；张量可以整体运算、赋值，要求形状和基类型匹配。

张量整体读取使用零个下标；元素访问必须提供与秩相同的下标个数。二维张量 a[0] 是部分下标，会被拒绝；a[0][1] 才是标量元素。

> @ 应该放加法那层还是乘法那层？我不想再弄出文法冲突。

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `frontend/parser/lexer.l`
> - Read `frontend/parser/yacc.y`
>
> </details>

</details>

按张量补充材料第 1 页，@ 放在 MulExp，与 * / % 同层。当前文法用左递归，因此 A@B*C 先做 A@B，再逐元素乘 C。

lexer.l 返回 MATMUL，yacc.y 构建 Operator::MATMUL；TensorType 由 TENSOR INT / TENSOR FLOAT 构造。添加规则后实际运行完整 Bison 诊断，而不是只依赖 make 没报错。

> 张量和标量能像普通 int+float 那样随便转换吗？float 张量能用 % 吗？

张量规则更严格：int 张量只能和 int 标量混合，float 张量只能和 float 标量混合；两个张量的基类型也必须一致。% 只接受 int 张量元素。比较、&&、||、! 都不接受张量。

整体赋值要求两侧都是同类型、同形状张量，而且左侧可写；不支持 a=3，也不支持把整个张量直接赋给标量。元素 a[i] 已是普通标量时，按原标量规则处理。

> 手册说初始化层级匹配，但又给二维平铺的值，按哪个写？

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `frontend/ast/visitor/sementic_check/decl_checker.cpp`
> - Read `frontend/ast/visitor/sementic_check/tensor_checker.cpp`
> - Read `middleend/visitor/codegen/decl_codegen.cpp`
>
> </details>

</details>

材料同时给出嵌套与展平写法，因此实现按明确给出的写法兼容数组的展平和补零规则：

```c
tensor int a[2][3]={{1,2},{3}};
tensor float b[2][2]={1.0,2.0,3.0,4.0};
```

a 得到 [[1,2,0],[3,0,0]]。局部列表先清零再存显式值；整体表达式初值另走 tensorEval 和 tensorCopy。全局整体初值从已知初始化数据计算，运行期计算则总是读取当前内存，不能用全局初值替代修改后的值。

> 函数参数写 []，函数里面从哪儿得到真正长度？
>
> ```c
> void add(tensor int a[]){a=a+2;}
> ```

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `middleend/visitor/codegen/expr_codegen.cpp`
> - Read `middleend/visitor/codegen/stmt_codegen.cpp`
>
> </details>

</details>

源语言一个张量参数在 LLVM 中降为两个参数：ptr 与隐藏的首维 i32。调用者传入长度 3 或 5，函数循环范围就分别是 3 或 5；尾维由声明中的常量决定。

未知首维参加整体运算或复制时，需要比较实际尺寸，不相等则进入 llvm.trap。只检查秩、然后假定首维一定相同，会产生越界读写。

> tensor int make(){tensor int r[2]={3,4};return r;} 能像数组一样 return 地址吗？

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `frontend/ast/stmt.h`
> - Read `frontend/ast/visitor/sementic_check/tensor_checker.cpp`
> - Read `middleend/visitor/codegen/stmt_codegen.cpp`
>
> </details>

</details>

不能返回已失效的局部栈地址。调用者先按固定返回形状分配结果缓冲，并将 ptr 作为隐藏首参数；被调用者在 return 前把元素复制进去，LLVM 签名返回 void。

多个 return 必须保持类型与形状一致。返回尺寸必须编译期已知，直接返回未知首维形参会报错。为支持递归调用写在基本分支之前，先预读完整函数中的固定返回形状，再正式检查所有节点。

> 整体赋值为什么还有快照？a=a@a 不是右边先算吗？

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `middleend/visitor/codegen/tensor_codegen.cpp`
>
> </details>

</details>

右边运算产生独立结果，赋值也按完整值复制。先保存右值，能覆盖别名参数和其他赋值组合，避免写目标时覆盖还没读完的源。

@ 的输出元素按行列点积计算；逐元素乘只读对应位置。手册矩阵用例的实际结果为 58、64、139、154，另有 a=a@a 与源对象修改后目标不变的回归。

> 新检出一 make 就缺这个，原来不是能编译吗？
>
> ```text
> fatal error: FlexLexer.h: No such file or directory
> ```

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `Makefile`
> - Read `frontend/parser/FlexLexer.h`
> - Ran:
>
> ```bash
> docker exec lab3-tensor-check bash -lc 'make -j6 > /evidence/build2.log 2>&1; result=$?; tail -25 /evidence/build2.log; exit $result'
> ```
>
> ```text
> Linking object files -> bin/compiler
> ```
>
> </details>

</details>

新建课程镜像没有原环境里的额外头文件，但仓库自带 frontend/parser/FlexLexer.h。Makefile 的 INC_DIR 加入 frontend/parser 后，生成器与 scanner 使用同一份仓库头文件；不需要复制旧 obj/bin，也不用改变官方测试。

后续严格构建通过。干净检出能暴露原目录掩盖的环境依赖，这也是最后还要从提交重新构建的原因。

> 循环里一直做 a=a+1，如果首维是动态的，临时空间是不是每次都分配？

临时 alloca 不能放在循环每次经过的块。本实现维护 allocationBlock，把只依赖参数首维和静态尾维的计数、容量检查与分配放进函数序言，保存并恢复它到函数体的跳转边。

张量循环本身只做 load、算术和 store。tensor_dynamic_loop 连续执行 10000 次动态长度加法；另有乘积容量超过限制的拒绝测试，确保在分配前检查。

> 补返回形状预读后，编译怎么又说 ForStmt 没有 retType？

插入位置过宽，把函数专属的预读也放进了 while/for 的访问函数。已改为只在 FuncDeclStmt 的参数登记后调用。

同时修正测试驱动：构建或主回归失败时立即退出，不能继续用上一次的二进制测试。那次失败的日志保留，后面的成功结论来自重新构建后的运行。

> 逐块看生成器时发现 while/for 也改了 allocationBlock，这会不会又把临时空间放回循环？

会。分配块应只在函数入口维护，循环访问者不能把它改成循环体。已移除 while/for 中误插的序言更新，只保留 FuncDeclStmt 里的入口设置，并重新生成真实源码差异。修正后重新执行全套回归，87 个语义负例、52 个执行正例、两组各 100 个官方用例及 4 个运行期拒绝例全部通过；日志位于 test4。

> 现在怎么一次验证旧功能和 tensor？官方测试没有更新，那新增部分怎么证明？

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `/private/tmp/lab3-tensor-20261125/test3/regression.log`
> - Ran:
>
> ```bash
> docker exec lab3-tensor-check bash -lc 'python3 testcase/tensor/run.py --native --official --logs /evidence/test3 > /evidence/test3-summary.log 2>&1; result=$?; tail -30 /evidence/test3-summary.log; exit $result'
> ```
>
> ```text
> Semantic negatives: 87, execution cases: 52, failures: []
> Official Basic: Passed: 100 / 100, pass=True
> Official Advanced: Passed: 100 / 100, pass=True
> Tensor runtime shape rejections: 4/4; errors: []
> ```
>
> </details>

</details>

执行：

```bash
python3 testcase/tensor/run.py --native --official --logs /tmp/lab3-tensor-results
```

原脚本与官方输入、答案保持不变。驱动增加 41 个张量语义负例、32 个执行正例，以及 4 个运行期形状/容量拒绝例。正例需要编译、LLVM 检查、链接全部成功，再比较 stdout 和返回值；运行期拒绝例则要求前面都成功、执行非零。

包含旧回归后的最新汇总是：

```text
Semantic negatives: 87, execution cases: 52, failures: []
Official Basic: Passed: 100 / 100, pass=True
Official Advanced: Passed: 100 / 100, pass=True
Tensor runtime shape rejections: 4/4; errors: []
```

环境为 Linux ARM64、LLVM14、-O0。这些结果不代表已经通过课程 OJ 或完成后续优化/后端。

> Bison 新增 @ 后的诊断是什么？

<details><summary>1 previous message</summary>

> 我先核对这部分源码和对应验证结果。
>
> <details><summary>Explored files and commands</summary>
>
> - Read `/private/tmp/lab3-tensor-20261125/bison/yacc.output`
> - Ran:
>
> ```bash
> bison -d -v -Wall -Wcounterexamples -Werror=conflicts-sr -Werror=conflicts-rr --report=all yacc.y
> ```
>
> ```text
> Conflict between rule 29 and token ELSE resolved as shift (THEN < ELSE).
> ```
>
> </details>

</details>

Bison 3.8.2 的完整报告退出 0，未解决移进/归约、归约/归约冲突均 0，没有无用规则或非终结符；11 个未使用 token，1 处 THEN<ELSE 已消解移进冲突。

这仍不能把原始 CFG 称为形式化无二义性。@ 的同层左结合由 tensor_precedence 验证，原来的就近 else 与花括号规则也包含在旧回归里。

> 原目录还有我没提交的 yacc.y 注释，别把它一起提交。新增代码放在哪里？

为此使用独立检出 /Users/peng/.codex/worktrees/lab3-tensor/NKU-Compiler2026，分支 lab3-tensor，从已完成 Lab3 的 b05fc27 开始。原主工作区未提交的注释、文档和临时样例保持原样。

新检出显式暂存张量实现、必要构建修复、测试与两份文档；保留 /doc/ 忽略规则，已跟踪文档仍能更新。不会把 Python 缓存或编译产物加入提交。

> 先停在本地，最后还需要核对什么？

检查暂存范围和源码摘要，再从最终本地提交导出独立新目录，用同一条驱动命令重新构建并验证。这样不依赖当前检出的 obj/bin 或未跟踪文件。

学习文档绑定的是前后源码快照，提交编号留在交付信息中。按手册第 3 页，课程另需小雅提交 GitLab 链接和课堂小组讲解；本次不执行 push。
