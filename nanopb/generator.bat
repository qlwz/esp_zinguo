cd /d %~dp0

D:\Workspaces_Smarthome\esp\nanopb\generator-bin\protoc.exe --nanopb_out=. ZinguoConfig.proto
copy ZinguoConfig.pb.h ..\include /y
copy ZinguoConfig.pb.c ..\src /y
del ZinguoConfig.pb.h
del ZinguoConfig.pb.c
