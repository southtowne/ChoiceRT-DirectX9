# ChoiceRT-DirectX9
Measures choice reaction time at high framerate.

![GitHub Release Downloads](https://img.shields.io/github/downloads/southtowne/ChoiceRT-DirectX9/total)


# Build Instructions

1. Prerequisites: Windows 10/11 x64, Visual Studio 2022 or later with the "Desktop development with C++" workload (https://visualstudio.microsoft.com/downloads/) — this includes the Windows 10 SDK and MSBuild
2. Clone: git clone https://github.com/<your-username>/ChoiceRT-DirectX9.git
3. Build: Open a Developer Command Prompt in ChoiceRT-DirectX9 and run: msbuild ChoiceRT-DirectX9.sln -p:Configuration=Release -p:Platform=x64
4. Output: The finished exe lands at x64\Release\ChoiceRT-DirectX9.exe
5. Run: Right-click ChoiceRT-DirectX9.exe & run as administrator
