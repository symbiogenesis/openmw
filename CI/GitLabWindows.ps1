param(
    [String] $GitLabUserName,
    [String] $CIProjectDir,
    [String] $CIBuildRefName,
    [String] $CIBuildID
)

$Time = (Get-Date -Format "%H:%m")
echo $Time
echo "started by ${$GitLabUserName}"
$MemoryLogger = Start-Process powershell CI\MemoryLogger.ps1 -RedirectStandardOutput memorylog.log -PassThru
cd $CIProjectDir
sh CI/before_script.msvc.sh -c Release -p Win64 -v 2019 -k -V
cmake --build MSVC2019_64 --target ALL_BUILD --config Release
echo "Exit status $?"
cd MSVC2019_64\Release
7z a -tzip ..\..\OpenMW_MSVC2019_64_${$CIBuildRefName}_${$CIBuildID}.zip '*'
Stop-Process $MemoryLogger
