Write-Host "Total memory: $((Get-WmiObject win32_operatingsystem -ComputerName $env:COMPUTERNAME).TotalVisibleMemorySize)"
while ($True) {
	Start-Sleep -Seconds 1
	$Computer = Get-WmiObject win32_operatingsystem -ComputerName $env:COMPUTERNAME
	Write-Host "Used memory at $(date -Format '%H:%m'): $($Computer.TotalVisibleMemorySize - $Computer.FreePhysicalMemory) / $($Computer.TotalVisibleMemorySize)"
}
