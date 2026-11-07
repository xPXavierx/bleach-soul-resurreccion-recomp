#!/bin/sh
# Builds the installer stub (Instalador.exe); montar-instalador.py appends the program files to it.
# "compilar-instalador.sh teste" builds Instalador-teste.exe, which runs without elevation (asInvoker), for tests.
cd "$(dirname "$0")"
F=/c/Windows/Microsoft.NET/Framework64/v4.0.30319
MAN=instalador.manifest; OUT=Instalador.exe
if [ "$1" = teste ]; then sed 's/requireAdministrator/asInvoker/' instalador.manifest > instalador-teste.manifest; MAN=instalador-teste.manifest; OUT=Instalador-teste.exe; fi
$F/csc.exe //nologo //target:winexe //codepage:65001 //win32icon:icone-recomp.ico "//win32manifest:$MAN" \
  //r:System.Windows.Forms.dll //r:System.Drawing.dll //r:System.Web.Extensions.dll "//lib:$(cygpath -w $F/WPF)" //r:UIAutomationClient.dll //r:UIAutomationTypes.dll \
  //r:System.IO.Compression.dll //r:System.IO.Compression.FileSystem.dll "//out:$OUT" Instalador.cs Compilacao.cs Nucleo.cs Visual.cs Idiomas.cs
