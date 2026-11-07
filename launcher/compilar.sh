#!/bin/sh
# Builds "Bleach Launcher.exe" (launcher) with the .NET Framework 4 C# compiler.
# "compilar.sh ultrawide" also shows the aspect ratio option (still in progress; the v1 release has none).
cd "$(dirname "$0")"
F=/c/Windows/Microsoft.NET/Framework64/v4.0.30319
DEF=
[ "$1" = ultrawide ] && DEF=//define:ULTRAWIDE
$F/csc.exe //nologo //target:winexe //codepage:65001 //win32icon:icone-recomp.ico $DEF \
  //r:System.Windows.Forms.dll //r:System.Drawing.dll //r:System.Web.Extensions.dll "//lib:$(cygpath -w $F/WPF)" //r:UIAutomationClient.dll //r:UIAutomationTypes.dll \
  "//out:Bleach Launcher.exe" Launcher.cs Nucleo.cs Visual.cs Idiomas.cs Traducao.cs
