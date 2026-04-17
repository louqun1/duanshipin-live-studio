param(
    [string]$ExePath = ".\build\apps\stage1_video_trace\stage1_video_trace.exe",
    [int]$FrameCount = 2,
    [ValidateSet("client", "server")]
    [string]$Mode = "client",
    [int]$Port = 0,
    [string]$SummaryFile = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Read-Exact {
    param($Stream, [int]$Count)
    $buffer = New-Object byte[] $Count
    $offset = 0
    while ($offset -lt $Count) {
        $read = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($read -le 0) {
            throw "expected $Count bytes, got $offset"
        }
        $offset += $read
    }
    return $buffer
}

function Read-UInt24BE {
    param([byte[]]$Bytes, [int]$Offset)
    return ([int]$Bytes[$Offset] -shl 16) -bor ([int]$Bytes[$Offset + 1] -shl 8) -bor [int]$Bytes[$Offset + 2]
}

function Add-Bytes {
    param($List, [byte[]]$Bytes)
    foreach ($byteValue in $Bytes) {
        $List.Add([byte]$byteValue)
    }
}

function Add-UInt16BE {
    param($List, [int]$Value)
    $List.Add([byte](($Value -shr 8) -band 0xFF))
    $List.Add([byte]($Value -band 0xFF))
}

function Add-UInt24BE {
    param($List, [int]$Value)
    $List.Add([byte](($Value -shr 16) -band 0xFF))
    $List.Add([byte](($Value -shr 8) -band 0xFF))
    $List.Add([byte]($Value -band 0xFF))
}

function Add-UInt32BE {
    param($List, [uint32]$Value)
    $List.Add([byte](($Value -shr 24) -band 0xFF))
    $List.Add([byte](($Value -shr 16) -band 0xFF))
    $List.Add([byte](($Value -shr 8) -band 0xFF))
    $List.Add([byte]($Value -band 0xFF))
}

function Add-UInt32LE {
    param($List, [uint32]$Value)
    $List.Add([byte]($Value -band 0xFF))
    $List.Add([byte](($Value -shr 8) -band 0xFF))
    $List.Add([byte](($Value -shr 16) -band 0xFF))
    $List.Add([byte](($Value -shr 24) -band 0xFF))
}

function New-AmfString {
    param([string]$Text)
    $list = [System.Collections.Generic.List[byte]]::new()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    $list.Add(0x02)
    Add-UInt16BE $list $bytes.Length
    Add-Bytes $list $bytes
    return $list.ToArray()
}

function New-AmfNumber {
    param([double]$Value)
    $list = [System.Collections.Generic.List[byte]]::new()
    $list.Add(0x00)
    $bytes = [BitConverter]::GetBytes([double]$Value)
    if ([BitConverter]::IsLittleEndian) {
        [Array]::Reverse($bytes)
    }
    Add-Bytes $list $bytes
    return $list.ToArray()
}

function New-AmfNull {
    return [byte[]](0x05)
}

function New-AmfObject {
    param([object[]]$Pairs)
    $list = [System.Collections.Generic.List[byte]]::new()
    $list.Add(0x03)
    foreach ($pair in $Pairs) {
        $nameBytes = [System.Text.Encoding]::UTF8.GetBytes([string]$pair[0])
        Add-UInt16BE $list $nameBytes.Length
        Add-Bytes $list $nameBytes
        Add-Bytes $list ([byte[]]$pair[1])
    }
    $list.Add(0x00)
    $list.Add(0x00)
    $list.Add(0x09)
    return $list.ToArray()
}

function Join-Bytes {
    param([object[]]$Parts)
    $list = [System.Collections.Generic.List[byte]]::new()
    foreach ($part in $Parts) {
        Add-Bytes $list ([byte[]]$part)
    }
    return $list.ToArray()
}

function Read-AmfStringAt {
    param([byte[]]$Payload, [ref]$Offset)
    if ($Payload[$Offset.Value] -ne 0x02) {
        throw "expected AMF0 string marker at offset $($Offset.Value)"
    }
    $Offset.Value += 1
    $length = ([int]$Payload[$Offset.Value] -shl 8) -bor [int]$Payload[$Offset.Value + 1]
    $Offset.Value += 2
    $text = [System.Text.Encoding]::UTF8.GetString($Payload, $Offset.Value, $length)
    $Offset.Value += $length
    return $text
}

function Read-FirstAmfString {
    param([byte[]]$Payload)
    $offset = 0
    return Read-AmfStringAt $Payload ([ref]$offset)
}

function Read-FirstTwoAmfStrings {
    param([byte[]]$Payload)
    $offset = 0
    $first = Read-AmfStringAt $Payload ([ref]$offset)
    $second = Read-AmfStringAt $Payload ([ref]$offset)
    return @($first, $second)
}

function Send-RtmpMessage {
    param($Stream, [int]$ChunkStreamId, [int]$Timestamp, [int]$TypeId, [int]$MessageStreamId, [byte[]]$Payload)
    $list = [System.Collections.Generic.List[byte]]::new()
    $list.Add([byte]$ChunkStreamId)
    Add-UInt24BE $list ([Math]::Min($Timestamp, 0xFFFFFF))
    Add-UInt24BE $list $Payload.Length
    $list.Add([byte]$TypeId)
    Add-UInt32LE $list ([uint32]$MessageStreamId)
    if ($Timestamp -ge 0xFFFFFF) {
        Add-UInt32BE $list ([uint32]$Timestamp)
    }
    Add-Bytes $list $Payload
    $bytes = $list.ToArray()
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush()
}

function Read-AmfValue {
    param([byte[]]$Payload, [ref]$Offset)
    $marker = $Payload[$Offset.Value]
    $Offset.Value += 1
    switch ($marker) {
        0x00 {
            $bytes = $Payload[$Offset.Value..($Offset.Value + 7)]
            if ([BitConverter]::IsLittleEndian) {
                [Array]::Reverse($bytes)
            }
            $Offset.Value += 8
            return [BitConverter]::ToDouble($bytes, 0)
        }
        0x01 {
            $value = $Payload[$Offset.Value] -ne 0
            $Offset.Value += 1
            return $value
        }
        0x02 {
            $length = ([int]$Payload[$Offset.Value] -shl 8) -bor [int]$Payload[$Offset.Value + 1]
            $Offset.Value += 2
            $text = [System.Text.Encoding]::UTF8.GetString($Payload, $Offset.Value, $length)
            $Offset.Value += $length
            return $text
        }
        0x03 {
            $object = @{}
            while ($true) {
                $length = ([int]$Payload[$Offset.Value] -shl 8) -bor [int]$Payload[$Offset.Value + 1]
                if ($length -eq 0 -and $Payload[$Offset.Value + 2] -eq 0x09) {
                    $Offset.Value += 3
                    return $object
                }
                $Offset.Value += 2
                $key = [System.Text.Encoding]::UTF8.GetString($Payload, $Offset.Value, $length)
                $Offset.Value += $length
                $object[$key] = Read-AmfValue $Payload ([ref]$Offset.Value)
            }
        }
        0x05 {
            return $null
        }
        0x08 {
            $Offset.Value += 4
            $object = @{}
            while ($true) {
                $length = ([int]$Payload[$Offset.Value] -shl 8) -bor [int]$Payload[$Offset.Value + 1]
                if ($length -eq 0 -and $Payload[$Offset.Value + 2] -eq 0x09) {
                    $Offset.Value += 3
                    return $object
                }
                $Offset.Value += 2
                $key = [System.Text.Encoding]::UTF8.GetString($Payload, $Offset.Value, $length)
                $Offset.Value += $length
                $object[$key] = Read-AmfValue $Payload ([ref]$Offset.Value)
            }
        }
        default {
            throw "unsupported AMF marker $marker"
        }
    }
}

function Parse-AmfSequence {
    param([byte[]]$Payload)
    $values = @()
    $offset = 0
    while ($offset -lt $Payload.Length) {
        $values += ,(Read-AmfValue $Payload ([ref]$offset))
    }
    return ,$values
}

function Read-ClientMessage {
    param($Stream, [ref]$StateMap, [ref]$ChunkSize)
    while ($true) {
        $first = (Read-Exact $Stream 1)[0]
        $format = $first -shr 6
        $chunkStreamId = $first -band 0x3F
        if ($chunkStreamId -eq 0) {
            $chunkStreamId = 64 + (Read-Exact $Stream 1)[0]
        } elseif ($chunkStreamId -eq 1) {
            $extension = Read-Exact $Stream 2
            $chunkStreamId = 64 + $extension[0] + ($extension[1] -shl 8)
        }

        if (-not $StateMap.Value.ContainsKey($chunkStreamId)) {
            $StateMap.Value[$chunkStreamId] = @{}
        }
        $state = $StateMap.Value[$chunkStreamId]

        if ($format -eq 0) {
            $header = Read-Exact $Stream 11
            $timestamp = Read-UInt24BE $header 0
            $messageLength = Read-UInt24BE $header 3
            $typeId = [int]$header[6]
            $messageStreamId = [BitConverter]::ToUInt32($header, 7)
            if ($timestamp -eq 0xFFFFFF) {
                $timestampBytes = Read-Exact $Stream 4
                if ([BitConverter]::IsLittleEndian) {
                    [Array]::Reverse($timestampBytes)
                }
                $timestamp = [BitConverter]::ToUInt32($timestampBytes, 0)
            }
            $state.timestamp = $timestamp
            $state.messageLength = $messageLength
            $state.typeId = $typeId
            $state.messageStreamId = $messageStreamId
            $state.payload = [System.Collections.Generic.List[byte]]::new()
            $state.read = 0
        } elseif ($format -ne 3) {
            throw "unsupported client chunk format $format"
        }

        $remaining = $state.messageLength - $state.read
        $take = [Math]::Min($ChunkSize.Value, $remaining)
        $chunk = Read-Exact $Stream $take
        Add-Bytes $state.payload $chunk
        $state.read += $take

        if ($state.read -eq $state.messageLength) {
            $payload = $state.payload.ToArray()
            $message = [pscustomobject]@{
                ChunkStreamId = $chunkStreamId
                Timestamp = $state.timestamp
                TypeId = $state.typeId
                MessageStreamId = $state.messageStreamId
                Payload = $payload
            }
            if ($message.TypeId -eq 1 -and $payload.Length -eq 4) {
                $reversed = [byte[]]$payload.Clone()
                [Array]::Reverse($reversed)
                $ChunkSize.Value = [BitConverter]::ToUInt32($reversed, 0) -band 0x7FFFFFFF
            }
            return $message
        }
    }
}

function Run-FakeRtmpServer {
    param([int]$ListenPort)
    $summary = [System.Collections.Generic.List[string]]::new()
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $ListenPort)
    $listener.Start()
    try {
        $client = $listener.AcceptTcpClient()
        $stream = $client.GetStream()
        try {
            $c0c1 = Read-Exact $stream 1537
            $summary.Add("handshake_c0=$($c0c1[0])")

            $reply = New-Object byte[] (1 + 1536 + 1536)
            $reply[0] = 3
            $stream.Write($reply, 0, $reply.Length)
            $stream.Flush()
            [void](Read-Exact $stream 1536)

            $stateMap = @{}
            $chunkSize = 128

            $message = Read-ClientMessage $stream ([ref]$stateMap) ([ref]$chunkSize)
            $summary.Add("cmd1=$(Read-FirstAmfString $message.Payload)")

            $connectResult = [byte[]](
                0x02,0x00,0x07,0x5F,0x72,0x65,0x73,0x75,0x6C,0x74,
                0x00,0x3F,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,
                0x03,0x00,0x00,0x09,
                0x03,0x00,0x00,0x09
            )
            Send-RtmpMessage $stream 3 0 20 0 $connectResult

            $message = Read-ClientMessage $stream ([ref]$stateMap) ([ref]$chunkSize)
            $summary.Add("cmd2=$(Read-FirstAmfString $message.Payload)")

            $createStreamResult = [byte[]](
                0x02,0x00,0x07,0x5F,0x72,0x65,0x73,0x75,0x6C,0x74,
                0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                0x05,
                0x00,0x3F,0xF0,0x00,0x00,0x00,0x00,0x00,0x00
            )
            Send-RtmpMessage $stream 3 0 20 0 $createStreamResult

            $message = Read-ClientMessage $stream ([ref]$stateMap) ([ref]$chunkSize)
            $summary.Add("cmd3=$(Read-FirstAmfString $message.Payload)")

            $publishStart = [byte[]](
                0x02,0x00,0x08,0x6F,0x6E,0x53,0x74,0x61,0x74,0x75,0x73,
                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                0x05,
                0x03,
                0x00,0x04,0x63,0x6F,0x64,0x65,
                0x02,0x00,0x17,0x4E,0x65,0x74,0x53,0x74,0x72,0x65,0x61,0x6D,0x2E,0x50,0x75,0x62,0x6C,0x69,0x73,0x68,0x2E,0x53,0x74,0x61,0x72,0x74,
                0x00,0x00,0x09
            )
            Send-RtmpMessage $stream 5 0 20 1 $publishStart

            $videoCount = 0
            $metadataSeen = $false
            while ($videoCount -lt 3) {
                $message = Read-ClientMessage $stream ([ref]$stateMap) ([ref]$chunkSize)
                if ($message.TypeId -eq 1) {
                    $reversed = [byte[]]$message.Payload.Clone()
                    [Array]::Reverse($reversed)
                    $value = [BitConverter]::ToUInt32($reversed, 0) -band 0x7FFFFFFF
                    $summary.Add("set_chunk_size=$value")
                } elseif ($message.TypeId -eq 18) {
                    $strings = Read-FirstTwoAmfStrings $message.Payload
                    $summary.Add("metadata=$($strings[0])/$($strings[1])")
                    $metadataSeen = $true
                } elseif ($message.TypeId -eq 9) {
                    $videoCount += 1
                    $summary.Add("video$videoCount ts=$($message.Timestamp) bytes=$($message.Payload.Length) avcType=$($message.Payload[1])")
                }
            }
            $summary.Add("finished metadata=$metadataSeen video_messages=$videoCount")
        } finally {
            $stream.Dispose()
            $client.Dispose()
        }
    } finally {
        $listener.Stop()
    }
    return $summary
}

if ($Mode -eq "server") {
    if ($Port -le 0) {
        throw "server mode requires -Port"
    }
    if ([string]::IsNullOrWhiteSpace($SummaryFile)) {
        throw "server mode requires -SummaryFile"
    }
    try {
        $summary = Run-FakeRtmpServer -ListenPort $Port
        Set-Content -Path $SummaryFile -Value $summary -Encoding UTF8
    } catch {
        Set-Content -Path $SummaryFile -Value @("server_error=$($_.Exception.Message)") -Encoding UTF8
        throw
    }
    exit 0
}

$resolvedExe = Resolve-Path $ExePath
$port = Get-Random -Minimum 20000 -Maximum 40000
$summaryFile = Join-Path $env:TEMP ("rtmp_fake_summary_" + [guid]::NewGuid().ToString("N") + ".txt")
$serverArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $PSCommandPath,
    "-Mode", "server",
    "-Port", $port,
    "-SummaryFile", $summaryFile
)
$server = Start-Process -FilePath "powershell" -ArgumentList $serverArgs -PassThru -WindowStyle Hidden

try {
    Start-Sleep -Milliseconds 400
    $url = "rtmp://127.0.0.1:$port/live/stage1-trace"
    & $resolvedExe $url $FrameCount
    $clientExit = $LASTEXITCODE

    $null = $server.WaitForExit(10000)
    $summary = if (Test-Path $summaryFile) { Get-Content $summaryFile } else { @("server_error=no summary file") }

    Write-Output "client_exit=$clientExit"
    $summary

    if ($clientExit -ne 0) {
        exit $clientExit
    }
} finally {
    if (-not $server.HasExited) {
        $server.Kill()
    }
    if (Test-Path $summaryFile) {
        Remove-Item $summaryFile -Force
    }
}
