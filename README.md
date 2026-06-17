# cc

A C compiler. Build with:

```
cc srcgen.c -o srcgen
./srcgen
cc cc.c -o cc
```
 
> WINDOWS: Add ".exe" to -o path (i.e "-o cc.exe"). If you get linker errors add `-lntdll` and/or `-lkernel32`.
