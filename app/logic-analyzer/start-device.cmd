@echo off
rem 网口 2 的独立装置入口；页面启停只控制显示。
call "%~dp0start.cmd" --device http://192.168.2.100:8102 %*
