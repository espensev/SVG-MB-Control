Option Explicit

Dim shell, fso, scriptDir, psPath, watchdogScript, command, exitCode

Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)
psPath = shell.ExpandEnvironmentStrings("%SystemRoot%") & "\System32\WindowsPowerShell\v1.0\powershell.exe"
watchdogScript = scriptDir & "\Install-SVG-MB-ControlWatchdogScheduledTask.ps1"

command = """" & psPath & """ -NoProfile -NonInteractive -ExecutionPolicy Bypass -File """ & watchdogScript & """ -Run"
exitCode = shell.Run(command, 0, True)

WScript.Quit exitCode
