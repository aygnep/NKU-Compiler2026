%{
    #include <frontend/parser/yacc.h>
    #include <frontend/parser/scanner.h>
    #include <frontend/parser/location.hh>
    #include <frontend/parser/parser.h>

    #include <cerrno>
    #include <cstdlib>
    #include <limits>
    #include <stdexcept>
    #include <string>

    using namespace FE;

    static location loc;
    // YY_USER_ACTION 宏会自动插入到每个规则的代码块开头
    // 如下方的 "int"               { RETT(INT, loc) }
    // 实际上会被转换为
    // "int"               { loc.step(); loc.columns(yyleng); RETT(INT, loc) }
    // 这样就能自动更新 loc 的位置
    // yyleng 是当前匹配的字符串的长度
    #define YY_USER_ACTION      \
        loc.step();             \
        loc.columns(yyleng);

    #define yywrap() 1
    #define RETT(x, y) return YaccParser::make_##x(y)

    #define TAB_WIDTH 4

    int handleTab();
    long long convertToInt(const char* str);
    float convertToFloatDec(const char* str);
    float convertToFloatHex(const char* str);
%}

%option c++ noyywrap nodefault
%option yyclass="Scanner"
%option prefix="Yacc_"

%x COMMENT

D       [0-9]
H       [0-9a-fA-F]
ID      [A-Za-z_][A-Za-z0-9_]*
DECINT  (0|[1-9]{D}*)
OCTINT  0[0-7]*
HEXINT  0[xX]{H}+
DECEXP  [eE][+-]?{D}+
HEXEXP  [pP][+-]?{D}+
DECFLOAT (({D}+\.{D}*|\.{D}+){DECEXP}?|{D}+{DECEXP})
HEXFLOAT 0[xX](({H}+\.{H}*|\.{H}+)|{H}+){HEXEXP}
/*输入读到 count 时：
1. Flex 发现 count 符合 ID。
2. yytext 指向这次匹配到的原文，也就是 count。
3. 复制成 C++ 字符串。
4. 创建并返回一个 IDENT，附带字符串属性和位置信息 */
%%

    /* Newlines and horizontal whitespace are discarded but still advance loc. */
\r\n|\r|\n       { loc.lines(1); loc.step(); }
[ \f\v]+          { /* YY_USER_ACTION updates the column */ }
\t                 { loc.columns(handleTab() - yyleng); }

    /* Comments are discarded. COMMENT is exclusive so comment text cannot become
       identifiers, numbers, or operators. */
"//"[^\r\n]*      { /* discard through, but not including, the newline */ }
"/*"              { BEGIN(COMMENT); }
<COMMENT>"*/"     { BEGIN(INITIAL); }
<COMMENT>\r\n|\r|\n { loc.lines(1); loc.step(); }
<COMMENT>[^*\r\n]+ { /* discard comment text */ }
<COMMENT>"*"      { /* a star not followed by slash */ }
<COMMENT><<EOF>>  {
    _parser.reportError(loc, "unterminated block comment");
    BEGIN(INITIAL);
    return YaccParser::make_ERR_TOKEN("unterminated block comment", loc);
}

    /* Keywords must precede ID when the match length is equal. */
"tensor"           { RETT(TENSOR, loc); }
"@"                { RETT(MATMUL, loc); }
"const"            { RETT(CONST, loc); }
"int"              { RETT(INT, loc); }
"float"            { RETT(FLOAT, loc); }
"void"             { RETT(VOID, loc); }
"if"               { RETT(IF, loc); }
"else"             { RETT(ELSE, loc); }
"for"              { RETT(FOR, loc); }
"while"            { RETT(WHILE, loc); }
"continue"         { RETT(CONTINUE, loc); }
"break"            { RETT(BREAK, loc); }
"switch"           { RETT(SWITCH, loc); }
"case"             { RETT(CASE, loc); }
"goto"             { RETT(GOTO, loc); }
"do"               { RETT(DO, loc); }
"return"           { RETT(RETURN, loc); }

    /* Longest-match ordering keeps these operators ahead of their one-character
       prefixes. */
"<="               { RETT(LE, loc); }
">="               { RETT(GE, loc); }
"=="               { RETT(EQ, loc); }
"!="               { RETT(NE, loc); }
"&&"               { RETT(AND, loc); }
"||"               { RETT(OR, loc); }
"+="               { RETT(ADD_ASSIGN, loc); }
"-="               { RETT(SUB_ASSIGN, loc); }
"*="               { RETT(MUL_ASSIGN, loc); }
"/="               { RETT(DIV_ASSIGN, loc); }
"%="               { RETT(MOD_ASSIGN, loc); }

{HEXFLOAT}|{DECFLOAT} {
    try {
        float value = (yytext[1] == 'x' || yytext[1] == 'X')
                          ? convertToFloatHex(yytext)
                          : convertToFloatDec(yytext);
        return YaccParser::make_FLOAT_CONST(value, loc);
    } catch (const std::exception& e) {
        _parser.reportError(loc, e.what());
        auto text = std::string(yytext);
        return YaccParser::make_ERR_TOKEN(text, loc);
    }
}

{HEXINT}|{OCTINT}|{DECINT} {
    try {
        long long value = convertToInt(yytext);
        if (value > std::numeric_limits<int>::max())
            return YaccParser::make_LL_CONST(value, loc);
        return YaccParser::make_INT_CONST(static_cast<int>(value), loc);
    } catch (const std::exception& e) {
        _parser.reportError(loc, e.what());
        auto text = std::string(yytext);
        return YaccParser::make_ERR_TOKEN(text, loc);
    }
}

{ID}               {
    auto text = std::string(yytext);//yytext 指向 当前规则匹配到的原始文本。
    return YaccParser::make_IDENT(text, loc);
}

\"[^\"\r\n]*\"   {
    std::string text(yytext);
    text = text.substr(1, text.size() - 2);
    return YaccParser::make_STR_CONST(text, loc);
}

"+"                { RETT(PLUS, loc); }
"-"                { RETT(MINUS, loc); }
"*"                { RETT(STAR, loc); }
"/"                { RETT(SLASH, loc); }
"%"                { RETT(PERCENT, loc); }
"="                { RETT(ASSIGN, loc); }
"<"                { RETT(LT, loc); }
">"                { RETT(GT, loc); }
"!"                { RETT(NOT, loc); }
"("                { RETT(LPAREN, loc); }
")"                { RETT(RPAREN, loc); }
"["                { RETT(LBRACKET, loc); }
"]"                { RETT(RBRACKET, loc); }
"{"                { RETT(LBRACE, loc); }
"}"                { RETT(RBRACE, loc); }
","                { RETT(COMMA, loc); }
";"                { RETT(SEMICOLON, loc); }

.                  {
    auto text = std::string(yytext);
    _parser.reportError(loc, "illegal character: " + text);
    return YaccParser::make_ERR_TOKEN(text, loc);
}

<<EOF>>             { RETT(END, loc); }

%%

int handleTab()
{
    const int column = loc.begin.column;
    return TAB_WIDTH - ((column - 1) % TAB_WIDTH);
}

long long convertToInt(const char* str)
{
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(str, &end, 0);
    if (errno == ERANGE || end == str || *end != '\0')
        throw std::out_of_range(std::string("invalid integer literal: ") + str);
    return value;
}

float convertToFloatDec(const char* str)
{
    errno = 0;
    char* end = nullptr;
    const float value = std::strtof(str, &end);
    if (errno == ERANGE || end == str || *end != '\0')
        throw std::out_of_range(std::string("invalid decimal float literal: ") + str);
    return value;
}

float convertToFloatHex(const char* str)
{
    // strtof follows the C hexadecimal-floating-constant grammar, including
    // the mandatory binary exponent introduced by p/P.
    return convertToFloatDec(str);
}
