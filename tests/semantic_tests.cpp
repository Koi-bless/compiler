#include "test_support.hpp"

int main() {
    TestPipeline valid("const int n=4; int fact(int x){if(x<=1)return 1;return x*fact(x-1);} int main(){int n=fact(n);return n;}");
    check(valid.semantic.functions.size() == 2, "function analysis failed");
    expectCompileError([] { TestPipeline bad("int main(){break;return 0;}"); }, "only valid inside a loop");
    expectCompileError([] { TestPipeline bad("int f(){return 1;} void main(){return;}"); }, "main must have signature");
    expectCompileError([] { TestPipeline bad("int main(){const int x=1;x=2;return x;}"); }, "cannot assign to constant");
    expectCompileError([] { TestPipeline bad("int main(){return later();} int later(){return 1;}"); }, "undefined function");
    expectCompileError([] { TestPipeline bad("int main(){int x=1;const int y=x;return y;}"); }, "depends on non-constant");
    expectCompileError([] { TestPipeline bad("int f(){return 1;} const int x=0&&f(); int main(){return x;}"); }, "not a compile-time constant");
    expectCompileError([] { TestPipeline bad("int f(){return 1;}"); }, "must define exactly one int main");
    expectCompileError([] { TestPipeline bad("int main(){int x=1;int x=2;return x;}"); }, "duplicate declaration");
    expectCompileError([] { TestPipeline bad("int main(){return x;}"); }, "undeclared name");
    expectCompileError([] { TestPipeline bad("int f(int x){return x;} int main(){return f();}"); }, "wrong number of arguments");
    expectCompileError([] { TestPipeline bad("void f(){} int main(){if(f())return 1;return 0;}"); }, "cannot have void type");
    expectCompileError([] { TestPipeline bad("void f(){return 1;} int main(){return 0;}"); }, "void function cannot return a value");
    expectCompileError([] { TestPipeline bad("int f(){return;} int main(){return 0;}"); }, "must return a value");
    expectCompileError([] { TestPipeline bad("int a=1;int b=a;int main(){return b;}"); }, "depends on non-constant");
}
