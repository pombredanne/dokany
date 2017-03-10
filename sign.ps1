$files = Get-ChildItem -path Win32,x64,ARM -recurse -Include *.sys,*.cat,*.dlls
signtool sign /v /i "$env:CERTISSUER" /ac "$env:ADDITIONALCERT" /t http://timestamp.verisign.com/scripts/timstamp.dll $files
signtool sign /as /fd SHA256 /v /i "$env:CERTISSUER" /ac "$env:ADDITIONALCERT" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 $files

if (-not (Test-Path $env:EV_CERTISSUER))
{
	$files = Get-ChildItem -path Win32\Win10Release,x64\Win10Release -recurse -Include *.sys,*.cat,*.dll
	signtool sign /as /fd sha256 /tr http://timestamp.digicert.com /td sha256 /n "$env:EV_CERTISSUER" $files
}
