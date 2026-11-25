#!/usr/bin/env python3
"""Run old Lab3 checks and new tensor cases; append --native --official as needed."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module

if __name__ == '__main__':
    base = load(ROOT/'testcase/lab3/run.py', 'lab3_base')
    cases = load(Path(__file__).with_name('cases.py'), 'tensor_cases')
    base.NEGATIVE.update(cases.NEGATIVE)
    base.POSITIVE.update(cases.POSITIVE)
    status = base.main()
    if status: sys.exit(status) # 构建/回归失败后不使用旧二进制继续测试
    errors=[]
    with tempfile.TemporaryDirectory(prefix='tensor-runtime-') as temp:
        p=Path(temp)
        llvm=shutil.which('llvm-as'); clang=shutil.which('clang')
        v=base.run([llvm,'--version']).stdout
        af=['-opaque-pointers'] if 'version 14.' in v else []
        cf=['-mllvm','-opaque-pointers'] if af else []
        for name, text in cases.RUNTIME_ERRORS.items():
            source=p/(name+'.sy');source.write_text(text)
            ir=p/(name+'.ll'); binary=p/(name+'.bin')
            for command in [[ROOT/'bin/compiler','-llvm',source,'-o',ir], [llvm,*af,ir,'-o',p/'check.bc'],[clang,*cf,ir,'-static','-o',binary]]:
                r=base.run(command)
                if r.returncode: errors.append((name,r.stderr));break
            else:
                result=base.run([binary])
                if result.returncode==0:errors.append((name,'runtime shape mismatch was accepted'))
        print(f'Tensor runtime shape rejections: {len(cases.RUNTIME_ERRORS)-len(errors)}/{len(cases.RUNTIME_ERRORS)}; errors: {errors}')
    sys.exit(1 if status or errors else 0)
