Param(
    [Parameter(Mandatory=$true)][string] $GitLabUserName,
    [Parameter(Mandatory=$true)][string] $CIProjectDir,
    [Parameter(Mandatory=$true)][string] $CIBuildRefName,
    [Parameter(Mandatory=$true)][string] $CIBuildID
)

exit

Write-Host All unrecognised arguments:
$Args | ForEach-Object { Write-Host $_ }

$Successful = $true
New-Item -Path IncompleteBuild.txt

$Time = (Get-Date -Format "%HH:%mm:%ss")
echo $Time
echo "started by ${$GitLabUserName}"

$MemoryLogger = Start-Process powershell CI\MemoryLogger.ps1 -RedirectStandardOutput memorylog.log -PassThru

sh CI/before_script.msvc.sh -c Release -p Win64 -v 2019 -k -V
$Successful = $Successful -and $?
$InnerSuccess = $false
For ($i = 0; $i -lt 3 -and -not $InnerSuccess; $i++)
{
    cmake --build MSVC2019_64 --target ALL_BUILD --config Release -v
    $InnerSuccess = $InnerSuccess -or $?
}
$Successful = $Successful -and $InnerSuccess

Push-Location MSVC2019_64\Release
7z a -tzip ..\..\OpenMW_MSVC2019_64_${$CIBuildRefName}_${$CIBuildID}.zip '*'
Pop-Location

Stop-Process $MemoryLogger

Remove-Item -Path IncompleteBuild.txt
if ($Successful) {
    New-Item -Path SuccessfulBuild.txt
}
