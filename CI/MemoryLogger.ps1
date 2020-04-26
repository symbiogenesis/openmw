Write-Host "Total memory: $((Get-WmiObject win32_operatingsystem -ComputerName $env:COMPUTERNAME).TotalVisibleMemorySize)"
while ($True) {
	Start-Sleep -Seconds 5
	$Computer = Get-WmiObject win32_operatingsystem -ComputerName $env:COMPUTERNAME
	Write-Host Used memory $($Computer.TotalVisibleMemorySize - $Computer.FreePhysicalMemory)
}
