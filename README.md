# ChoiceRT-DirectX9
Measures choice reaction time at high framerate.

<img width="978" height="512" alt="ChoiceRT" src="https://github.com/user-attachments/assets/f00920ec-1a8c-455a-95f1-aa800b944bb7" />

![GitHub Release Downloads](https://img.shields.io/github/downloads/southtowne/ChoiceRT-DirectX9/total)

# Usage
1. Download [ChoiceRT-DirectX9](https://github.com/southtowne/Mouse-Plotter/releases/download/V1.0/MousePlotter.exe).
2. Right-click & run as administrator.

# Build Instructions

1. Prerequisites: Windows 10/11 x64, Visual Studio 2022 or later with the "Desktop development with C++" workload (https://visualstudio.microsoft.com/downloads/) — this includes the Windows 10 SDK and MSBuild
2. Clone: git clone https://github.com/<your-username>/ChoiceRT-DirectX9.git
3. Build: Open a Developer Command Prompt in ChoiceRT-DirectX9 and run: msbuild ChoiceRT-DirectX9.sln -p:Configuration=Release -p:Platform=x64
4. Output: The finished exe lands at x64\Release\ChoiceRT-DirectX9.exe
5. Run: Right-click ChoiceRT-DirectX9.exe & run as administrator
