# Inspect the unedited audio tracks emitted by this plugin. ffprobe duration
# alone does not catch an incorrect tkhd clock when mdhd/stts are valid.
function Assert-Mp4AudioTrackTiming {
    param([Parameter(Mandatory)][string]$Path)
    $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Path))
    try {
        function Read-U32([long]$Offset) {
            $stream.Position = $Offset
            $bytes = [byte[]]::new(4)
            if ($stream.Read($bytes, 0, 4) -ne 4) { throw 'Truncated MP4 integer.' }
            return [uint64]$bytes[0] * 16777216 + [uint64]$bytes[1] * 65536 + [uint64]$bytes[2] * 256 + [uint64]$bytes[3]
        }
        function Read-U64([long]$Offset) {
            return [decimal](Read-U32 $Offset) * 4294967296 + [decimal](Read-U32 ($Offset + 4))
        }
        function Read-Type([long]$Offset) {
            $stream.Position = $Offset
            $bytes = [byte[]]::new(4)
            if ($stream.Read($bytes, 0, 4) -ne 4) { throw 'Truncated MP4 type.' }
            return [Text.Encoding]::ASCII.GetString($bytes)
        }
        function Read-Boxes([long]$Start, [long]$End) {
            $position = $Start
            while ($position -lt $End) {
                if ($End - $position -lt 8) { throw 'Truncated MP4 box header.' }
                $size = Read-U32 $position
                $type = Read-Type ($position + 4)
                $header = 8
                if ($size -eq 1) { $size = Read-U64 ($position + 8); $header = 16 }
                if ($size -eq 0) { $size = $End - $position }
                if ($size -lt $header -or $size -gt $End - $position) { throw "Invalid MP4 box size: $type" }
                [pscustomobject]@{ Type = $type; Payload = $position + $header; End = $position + [long]$size }
                $position += [long]$size
            }
        }
        function Read-Clock($Box, [bool]$TrackHeader = $false) {
            $stream.Position = $Box.Payload
            $version = $stream.ReadByte()
            if ($version -notin @(0, 1)) { throw 'Unsupported MP4 timing version.' }
            $offset = if ($version -eq 1) { 20 } else { 12 }
            $durationOffset = if ($TrackHeader) { $offset + 8 } else { $offset + 4 }
            $width = if ($version -eq 1) { 8 } else { 4 }
            if ($Box.Payload + $durationOffset + $width -gt $Box.End) { throw 'Truncated MP4 clock.' }
            $duration = if ($version -eq 1) { Read-U64 ($Box.Payload + $durationOffset) } else { Read-U32 ($Box.Payload + $durationOffset) }
            [pscustomobject]@{ Duration = [decimal]$duration; Timescale = if ($TrackHeader) { 0 } else { Read-U32 ($Box.Payload + $offset) } }
        }
        $moov = @(Read-Boxes 0 $stream.Length | Where-Object Type -eq 'moov')
        if ($moov.Count -ne 1) { throw 'Expected exactly one moov box.' }
        $children = @(Read-Boxes $moov[0].Payload $moov[0].End)
        $mvhd = @($children | Where-Object Type -eq 'mvhd')
        if ($mvhd.Count -ne 1) { throw 'Expected exactly one mvhd box.' }
        $movie = Read-Clock $mvhd[0]
        if ($movie.Timescale -eq 0) { throw 'Movie timescale is zero.' }
        $audioCount = 0
        foreach ($track in @($children | Where-Object Type -eq 'trak')) {
            $parts = @(Read-Boxes $track.Payload $track.End)
            $mdia = @($parts | Where-Object Type -eq 'mdia')
            if ($mdia.Count -ne 1) { throw 'Expected one mdia per track.' }
            $mediaParts = @(Read-Boxes $mdia[0].Payload $mdia[0].End)
            $handler = @($mediaParts | Where-Object Type -eq 'hdlr')
            if ($handler.Count -ne 1 -or $handler[0].Payload + 12 -gt $handler[0].End) { throw 'Invalid track handler.' }
            if ((Read-Type ($handler[0].Payload + 8)) -ne 'soun') { continue }
            if (@($parts | Where-Object Type -eq 'edts').Count) { throw 'Audio edit lists are outside this plugin timing contract.' }
            $tkhd = @($parts | Where-Object Type -eq 'tkhd')
            $mdhd = @($mediaParts | Where-Object Type -eq 'mdhd')
            if ($tkhd.Count -ne 1 -or $mdhd.Count -ne 1) { throw 'Missing or duplicate audio clock.' }
            $trackClock = Read-Clock $tkhd[0] $true
            $mediaClock = Read-Clock $mdhd[0]
            if ($mediaClock.Timescale -eq 0) { throw 'Audio timescale is zero.' }
            $expected = [decimal]::Floor($mediaClock.Duration * $movie.Timescale / $mediaClock.Timescale)
            if ($trackClock.Duration -ne $expected) {
                throw "Audio tkhd duration uses the wrong clock: actual=$($trackClock.Duration), expected=$expected (movie timescale=$($movie.Timescale))."
            }
            $audioCount++
        }
        if ($audioCount -eq 0) { throw 'No audio track found for timing validation.' }
        [pscustomobject]@{ audio_tracks = $audioCount; movie_timescale = $movie.Timescale; status = 'passed' }
    }
    finally { $stream.Dispose() }
}
