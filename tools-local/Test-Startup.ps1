param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath,

    [ValidateSet('gamer', 'programmer', 'hacker', 'saved')]
    [string]$Mode = 'gamer',

    [ValidateRange(1, 300)]
    [int]$ObservationSeconds = 45
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$executable = Get-Item -LiteralPath $ExecutablePath
$processName = [System.IO.Path]::GetFileNameWithoutExtension($executable.Name)
if (Get-Process -Name $processName -ErrorAction SilentlyContinue) {
    throw "Close existing $processName instances before running the startup test."
}

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $executable.FullName
$startInfo.WorkingDirectory = $executable.DirectoryName
$startInfo.Arguments = if ($Mode -eq 'saved') { '' } else { "-$Mode" }
$startInfo.UseShellExecute = $true

$settingsPath = 'Software\suyu team\suyu\General'
$savedSettings = @{}
$settingsKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($settingsPath)
if ($null -ne $settingsKey) {
    try {
        foreach ($name in 'AppMode', 'RememberMode') {
            if ($settingsKey.GetValueNames() -contains $name) {
                $savedSettings[$name] = @{
                    Value = $settingsKey.GetValue($name)
                    Kind = $settingsKey.GetValueKind($name)
                }
            }
        }
    } finally {
        $settingsKey.Dispose()
    }
}

$process = $null
try {
    $process = [System.Diagnostics.Process]::Start($startInfo)
    if ($process.WaitForExit($ObservationSeconds * 1000)) {
        $exitCode = '{0:X8}' -f $process.ExitCode
        throw "Startup failed in $Mode mode: process exited with code 0x$exitCode."
    }

    $process.Refresh()
    if ($process.MainWindowHandle -eq [IntPtr]::Zero -or -not $process.Responding) {
        throw "Startup failed in $Mode mode: no responsive main window after $ObservationSeconds seconds."
    }

    [PSCustomObject]@{
        Mode = $Mode
        ObservationSeconds = $ObservationSeconds
        Responding = $process.Responding
        WindowTitle = $process.MainWindowTitle
    }
} finally {
    try {
        if ($null -ne $process -and -not $process.HasExited) {
            if (-not $process.CloseMainWindow() -or -not $process.WaitForExit(10000)) {
                $process.Kill()
                $process.WaitForExit()
            }
        }
    } finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
        $settingsKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($settingsPath, $true)
        if ($null -ne $settingsKey) {
            try {
                foreach ($name in 'AppMode', 'RememberMode') {
                    if ($savedSettings.ContainsKey($name)) {
                        $settingsKey.SetValue($name, $savedSettings[$name].Value, $savedSettings[$name].Kind)
                    } else {
                        $settingsKey.DeleteValue($name, $false)
                    }
                }
            } finally {
                $settingsKey.Dispose()
            }
        }
    }
}