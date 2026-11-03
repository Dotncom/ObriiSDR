# Independent fixtures: .NET AES ECB primitive + explicit Meshtastic nonce/CTR layout.
# All keys and messages below are public test data. No Obrii implementation is used.
param([string]$Output = 'src/meshtastic/vectors.tsv')
$ErrorActionPreference = 'Stop'
function HexBytes([string]$Hex) {
    if ($Hex.Length % 2) { throw 'Odd hex length.' }
    $Result = [byte[]]::new($Hex.Length / 2)
    for ($Index = 0; $Index -lt $Result.Length; $Index++) { $Result[$Index] = [Convert]::ToByte($Hex.Substring(2*$Index,2),16) }
    return ,$Result
}
function HexString([byte[]]$Bytes) { return ([BitConverter]::ToString($Bytes)).Replace('-','').ToLowerInvariant() }
function Little32([uint32]$Value) {
    return ,([byte[]]@(0..3 | ForEach-Object { [byte](($Value -shr (8*$_)) -band 255) }))
}
$Records = @('# name|key_hex|sender_hex|packet_id_hex|plaintext_hex|ciphertext_hex|radio_hex')
$Plain = HexBytes '0801120568656c6c6f'
foreach ($Size in @(16,32)) {
    $Key = if ($Size -eq 16) { HexBytes 'd4f1bb3a20290759f0bcffabcf4e6901' } else { [byte[]](0..31) }
    $Sender = [uint32]0x11223344
    $PacketId = [uint32]0x12345678
    $Nonce = [byte[]]::new(16)
    [Array]::Copy((Little32 $PacketId),0,$Nonce,0,4)
    [Array]::Copy((Little32 $Sender),0,$Nonce,8,4)
    $Cipher = [byte[]]::new($Plain.Length)
    $Aes = [Security.Cryptography.Aes]::Create()
    try {
        $Aes.Key = $Key; $Aes.Mode = [Security.Cryptography.CipherMode]::ECB; $Aes.Padding = [Security.Cryptography.PaddingMode]::None
        $Encryptor = $Aes.CreateEncryptor()
        try {
            for ($Offset = 0; $Offset -lt $Plain.Length; $Offset += 16) {
                $Block = [byte[]]::new(16)
                [void]$Encryptor.TransformBlock($Nonce,0,16,$Block,0)
                for ($Index = 0; $Index -lt [Math]::Min(16,$Plain.Length-$Offset); $Index++) {
                    $Cipher[$Offset+$Index] = $Plain[$Offset+$Index] -bxor $Block[$Index]
                }
                for ($Index=15; $Index -ge 12; $Index--) {
                    if ($Nonce[$Index] -lt 255) { $Nonce[$Index]++; break }
                    $Nonce[$Index]=0
                }
            }
        } finally { $Encryptor.Dispose() }
    } finally { $Aes.Dispose() }
    [byte]$Hash=0
    foreach ($Byte in ([Text.Encoding]::UTF8.GetBytes('LongFast') + $Key)) { $Hash=$Hash -bxor $Byte }
    $Radio=[byte[]](@(255,255,255,255) + (Little32 $Sender) + (Little32 $PacketId) + @(0x63,$Hash,0,0x44) + $Cipher)
    $Records += "aes$($Size*8)|$(HexString $Key)|11223344|12345678|$(HexString $Plain)|$(HexString $Cipher)|$(HexString $Radio)"
}
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Destination = [IO.Path]::GetFullPath((Join-Path $Workspace $Output))
if (-not $Destination.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture output must be in the workspace.' }
[IO.File]::WriteAllLines($Destination,$Records,[Text.UTF8Encoding]::new($false))
Write-Output "Independent vectors: $Destination"
