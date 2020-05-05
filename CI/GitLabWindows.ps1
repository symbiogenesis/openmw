Param(
    [Parameter(Mandatory=$true)][string] $GitLabUserName,
    [Parameter(Mandatory=$true)][string] $CIProjectDir,
    [Parameter(Mandatory=$true)][string] $CIBuildRefName,
    [Parameter(Mandatory=$true)][string] $CIBuildID,
    [Parameter(Mandatory=$true)][string] $Configuration
)

$Successful = $true
New-Item -Path IncompleteBuild.txt

$Time = (Get-Date -Format "%HH:%mm:%ss")
echo $Time
echo "started by ${$GitLabUserName}"

$MemoryLogger = Start-Process powershell CI\MemoryLogger.ps1 -RedirectStandardOutput memorylog.log -PassThru

sh CI/before_script.msvc.sh -c $Configuration -p Win64 -v 2019 -k -V
$Successful = $Successful -and $?
$InnerSuccess = $false
For ($i = 0; $i -lt 1 -and -not $InnerSuccess; $i++)
{
    cmake --build MSVC2019_64 --target ALL_BUILD --config $Configuration -v -- -fileLogger "-fileLoggerParameters:LogFile=MSBuild.log;Append;Verbosity=diagnostic;Encoding=UTF-8;ShowTimestamp"
    $InnerSuccess = $InnerSuccess -or $?
}
$Successful = $Successful -and $InnerSuccess

Push-Location MSVC2019_64\$Configuration
7z a -tzip ..\..\OpenMW_MSVC2019_64_${CIBuildRefName}_${CIBuildID}.zip '*'
Pop-Location

Stop-Process $MemoryLogger

Remove-Item -Path IncompleteBuild.txt
if ($Successful) {
    New-Item -Path SuccessfulBuild.txt
}
