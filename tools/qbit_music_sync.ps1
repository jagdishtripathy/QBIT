# ==========================================================================
#  QBIT Live Music Auto-Sync (Windows Media Session -> QBIT OLED Display)
#  Automatically detects music playing in Chrome, Edge, Brave, Firefox,
#  Spotify, or VLC on Windows and updates QBIT's spinning vinyl display.
#
#  Usage:
#    powershell.exe -ExecutionPolicy Bypass -File tools\qbit_music_sync.ps1
# ==========================================================================
param(
    [string]$QbitHost = "http://qbit.local",
    [int]$PollMs = 1500
)

# Re-launch under Windows PowerShell 5.1 if invoked from PowerShell 7 (pwsh)
# because WinRT GlobalSystemMediaTransportControls requires Desktop CLR.
if ($PSVersionTable.PSEdition -eq "Core") {
    Write-Host "[QBIT Music] Switching to Windows PowerShell 5.1 for WinRT Media Session support..." -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path -QbitHost $QbitHost -PollMs $PollMs
    exit $LASTEXITCODE
}

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq "AsTask" -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -like "IAsyncOperation*"
})[0]

function Await-WinRT($WinRtTask, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtTask))
    $netTask.Wait(-1) | Out-Null
    return $netTask.Result
}

[Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager,Windows.Media.Control,ContentType=WindowsRuntime] | Out-Null
$mgr = Await-WinRT ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " QBIT Live Music Auto-Sync running -> $QbitHost" -ForegroundColor Green
Write-Host " Play any song in your browser (YouTube/Spotify) or laptop!" -ForegroundColor Yellow
Write-Host " Press Ctrl+C to stop." -ForegroundColor Gray
Write-Host "==========================================================" -ForegroundColor Cyan

$lastStateKey = ""
$lastSyncEpoch = 0

while ($true) {
    try {
        $session = $mgr.GetCurrentSession()
        $isPlaying = $false
        $title = ""
        $artist = ""
        $posSec = 0
        $durSec = 180

        if ($session) {
            $pb = $session.GetPlaybackInfo()
            $status = $pb.PlaybackStatus.ToString()
            $isPlaying = ($status -eq "Playing")
            $info = Await-WinRT ($session.TryGetMediaPropertiesAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties])
            if ($info) {
                $title = if ($info.Title) { $info.Title.Trim() } else { "" }
                $artist = if ($info.Artist) { $info.Artist.Trim() } else { "" }
            }
            $tl = $session.GetTimelineProperties()
            if ($tl) {
                $posSec = [Math]::Max(0, [int]$tl.Position.TotalSeconds)
                $d = [int]$tl.EndTime.TotalSeconds
                if ($d -gt 0) { $durSec = $d }
            }
        }

        $nowEpoch = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        $stateKey = "$isPlaying|$title|$artist"
        $periodicRefresh = ($isPlaying -and (($nowEpoch - $lastSyncEpoch) -ge 5))

        if (($stateKey -ne $lastStateKey) -or $periodicRefresh) {
            $lastStateKey = $stateKey
            $lastSyncEpoch = $nowEpoch
            $playVal = if ($isPlaying) { "1" } else { "0" }
            $encTitle = [uri]::EscapeDataString($title)
            $encArtist = [uri]::EscapeDataString($artist)
            $url = "$QbitHost/api/music?playing=$playVal&title=$encTitle&artist=$encArtist&progress=$posSec&duration=$durSec"
            Invoke-RestMethod -Uri $url -Method Post -TimeoutSec 3 | Out-Null
            if ($isPlaying) {
                Write-Host "[PLAYING] $title - $artist ($posSec/$durSec s)" -ForegroundColor Green
            } else {
                Write-Host "[PAUSED]  $title - $artist" -ForegroundColor DarkYellow
            }
        }
    } catch {
        # Ignore transient network timeouts if QBIT is rebooting
    }
    Start-Sleep -Milliseconds $PollMs
}
