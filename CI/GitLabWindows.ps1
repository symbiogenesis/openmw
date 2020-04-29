param(
    [String] $GitLabUserName,
    [String] $CIProjectDir,
    [String] $CIBuildRefName,
    [String] $CIBuildID
)

$Successful = $True
New-Item -Path IncompleteBuild.txt

$Time = (Get-Date -Format "%H:%m")
echo $Time
echo "started by ${$GitLabUserName}"

$MemoryLogger = Start-Process powershell CI\MemoryLogger.ps1 -RedirectStandardOutput memorylog.log -PassThru

sh CI/before_script.msvc.sh -c Release -p Win64 -v 2019 -k -V
$Successful = $Successful -and $?
cmake --build MSVC2019_64 --target ALL_BUILD --config Release
$Successful = $Successful -and $?

Push-Location MSVC2019_64\Release
7z a -tzip ..\..\OpenMW_MSVC2019_64_${$CIBuildRefName}_${$CIBuildID}.zip '*'
Pop-Location

Stop-Process $MemoryLogger

Remove-Item -Path IncompleteBuild.txt
if ($Successful) {
    New-Item -Path SuccessfulBuild.txt
}
