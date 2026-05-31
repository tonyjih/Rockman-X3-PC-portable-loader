@echo off
setlocal

cl /LD /EHsc /O2 /DMMX3_ENABLE_LOG=1 ^
  ddraw_proxy.cpp ^
  mmx3_common.cpp ^
  mmx3_registry.cpp ^
  mmx3_cd.cpp ^
  user32.lib winmm.lib ^
  /link /DEF:ddraw_proxy.def /OUT:ddraw.dll

endlocal
