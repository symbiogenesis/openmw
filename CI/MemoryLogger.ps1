Write-Host "Total memory: $((Get-WmiObject win32_operatingsystem -ComputerName $env:COMPUTERNAME).TotalVisibleMemorySize)"
while ($True) {
	Start-Sleep -Seconds 1
	$Computer = Get-WmiObject win32_operatingsystem -ComputerName $env:COMPUTERNAME
	Write-Host "Used memory at $(Get-Date -Format '%HH:%mm'): $($Computer.TotalVisibleMemorySize - $Computer.FreePhysicalMemory) / $($Computer.TotalVisibleMemorySize)"
	Write-Host "Running interesting processes:"
	Get-Process cmake, link, cl, msbuild, powershell -ErrorAction SilentlyContinue | Sort-Object -Descending WS | Format-Table * | Out-String -Width 2048
}
