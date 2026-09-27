    function Number([string]$Value) { return [double]::Parse($Value,$invariant) }
    function Quantile([double[]]$Values,[double]$P) {
        $a=@($Values | Sort-Object); $x=($a.Count-1)*$P; $lo=[int][Math]::Floor($x); $hi=[Math]::Min($lo+1,$a.Count-1)
        return [double]$a[$lo]+($x-$lo)*([double]$a[$hi]-[double]$a[$lo])
    }
    function Close-Enough([double]$A,[double]$B) { return [Math]::Abs($A-$B) -le (1e-10*[Math]::Max([Math]::Abs($B),1e-12)) }
    function Map-Check($Row) {
        if ($Row.map_hex -notmatch '^[0-9a-f]{512}$') { throw 'Invalid context-map record.' }
        $k=[int]$Row.realized_k
        if ($k -lt 1 -or $k -gt 256) { throw 'Invalid table count.' }
        for($c=0;$c -lt 256;$c++) {
            if ([Convert]::ToInt32($Row.map_hex.Substring(2*$c,2),16) -ge $k) { throw 'Context table ID exceeds table count.' }
        }
    }
    function Static-Accounting($Row) {
        [long]$bits=[long]$Row.count_bits+[long]$Row.map_bits+[long]$Row.table_bits+[long]$Row.padding_bits+[long]$Row.payload_bits+8*[long]$Row.state_bytes
        if ($bits -ne 8*[long]$Row.codec_body_bytes) { throw 'Serialized size components do not reconcile.' }
        if ([int]$Row.padding_bits -lt 0 -or [int]$Row.padding_bits -gt 7) { throw 'Invalid padding.' }
    }
    function Check-Validation([string]$Prefix,$InputRow) {
        $rows=@(Import-Csv -LiteralPath ($Prefix+'.candidates.csv'))
        $sel=@(Import-Csv -LiteralPath ($Prefix+'.selected.csv'))
        if ($rows.Count -ne 96 -or $sel.Count -ne 17) { throw "Incomplete validation: $Prefix" }
        $keys=@{};$selectedKeys=@{}
        foreach($r in $rows) {
            $key="$($r.generator)|$($r.coder)|$($r.requested_k)"
            if ($keys.ContainsKey($key)) { throw "Duplicate candidate: $key" }; $keys[$key]=$true
            if ($r.status -ne 'PASS' -or $r.decoded_equal -ne '1' -or $r.original_decoder_equal -ne '1' -or $r.accounting_ok -ne '1') { throw "Failed candidate: $key" }
            if ([long]$r.original_bytes -ne [long]$InputRow.length -or [int]$r.realized_k -gt [int]$r.requested_k) { throw 'Candidate input or count mismatch.' }
            Map-Check $r; Static-Accounting $r

        }
        foreach($g in @('reassignment','brotli_native_policy','matched_merge','initialization_only')) {
            foreach($c in @('huffman','rans')) {
                $set=@($rows | Where-Object {$_.generator -eq $g -and $_.coder -eq $c})
                if (($set.requested_k -join ',') -ne '1,2,3,4,6,8,12,16,24,32,48,64') { throw 'Candidate family is incomplete.' }
            }
        }
        foreach($s in $sel) {
            if ($selectedKeys.ContainsKey($s.variant) -or $variants -notcontains $s.variant -or $s.status -ne 'PASS') { throw 'Invalid selected variant record.' }
            $selectedKeys[$s.variant]=$true
            if ([long]$s.original_bytes -ne [long]$InputRow.length -or [long]$s.codec_body_bytes -le 0) { throw 'Selected input/size mismatch.' }
            Map-Check $s
            if ($s.variant -ne 'adaptive_order1') { Static-Accounting $s }
            if ($s.stream_fnv64 -notmatch '^[0-9a-f]{16}$') { throw 'Missing stream fingerprint.' }
            if ($s.generator -ne 'fixed_reference') {
                $family=@($rows | Where-Object {$_.generator -eq $s.generator -and $_.coder -eq $s.coder})
                if ($s.variant -eq 'huff_single') { $family=@($family | Where-Object {$_.requested_k -eq '1'}) }
                $winner=$null;[decimal]$minimum=[decimal]::MaxValue
                foreach($r in $family) {
                    [decimal]$cost=0
                    if ($s.coder -eq 'huffman') {
                        $cost=[decimal]$r.payload_bits
                        if ($s.selection -eq '0') { $cost += [decimal]$r.count_bits+[decimal]$r.map_bits+[decimal]$r.table_bits }
                    } elseif ($s.selection -eq '0') { $cost=[decimal]$r.codec_body_bytes }
                    elseif ($s.selection -eq '1') { $cost=[decimal]$r.payload_bits }
                    else { $cost=([decimal]$r.count_bits+[decimal]$r.map_bits+[decimal]$r.table_bits+[decimal]$r.padding_bits+32)*256+[decimal]$r.estimated_payload_units }
                    if ($cost -lt $minimum) { $minimum=$cost;$winner=$r }
                }
                if ($null -eq $winner -or $s.codec_body_bytes -ne $winner.codec_body_bytes -or $s.realized_k -ne $winner.realized_k -or $s.requested_k -ne $winner.requested_k -or $s.map_hex -cne $winner.map_hex) { throw 'Selected candidate is not the documented first minimum.' }
                $expectedCount=12; if ($s.variant -eq 'huff_single') {$expectedCount=1}
                $expectedEmits=$expectedCount;if ($s.coder -eq 'huffman' -or $s.selection -eq '2') {$expectedEmits=1}
                if ([int]$s.candidate_count -ne $expectedCount -or [int]$s.emissions -ne $expectedEmits) { throw 'Encoder operation-count mismatch.' }
            }
            if ([long]$InputRow.block_size -eq 0) {
                $priorKey="$($InputRow.group)|$($InputRow.file)|$($s.variant)"
                if ($oldSelected.ContainsKey($priorKey)) {
                    $priorRow=$oldSelected[$priorKey]
                    foreach($priorField in @('codec_body_bytes','realized_k','requested_k','emissions','candidate_count','decoder_table_bytes','stream_fnv64','map_hex')) {
                        if ($s.$priorField -cne $priorRow.$priorField) { throw "DCC004 preserved result changed: $priorKey / $priorField" }
                    }
                }
            }
        }
        if ($selectedKeys.Count -ne $variants.Count) { throw 'Missing selected variant.' }
        return $sel
    }
    function Check-Memory([string]$Prefix,$Expected) {
        $rows=@(Import-Csv -LiteralPath ($Prefix+'.memory.csv'));$seen=@{}
        if ($rows.Count -ne 17) { throw 'Incomplete memory record.' }
        foreach($r in $rows) {
            if ($seen.ContainsKey($r.variant) -or $variants -notcontains $r.variant -or $r.status -ne 'PASS') { throw 'Memory record failure.' };$seen[$r.variant]=$true
            $e=@($Expected | Where-Object {$_.variant -eq $r.variant})[0]
            foreach($f in @('original_bytes','codec_body_bytes','requested_k','realized_k','stream_fnv64','decoder_table_bytes')) {
                if ($r.$f -cne $e.$f) { throw "Memory build changed $f in $($r.variant)" }
            }
            if ($r.encode_after_release_delta -ne '0' -or $r.decode_after_return_delta -ne '0' -or $r.decode_peak_requested_heap_bytes -ne $r.decoder_table_bytes -or [long]$r.encode_peak_requested_heap_bytes -lt [long]$r.encode_return_live_bytes) { throw 'Heap accounting or release check failed.' }
        }
        return $rows
    }
    function Check-Loaded([string]$Prefix,$Members) {
        $loaded=@(Import-Csv -LiteralPath ($Prefix+'.loaded.csv'))
        if ($loaded.Count -ne $Members.Count) { throw 'Loaded member count mismatch.' }
        for($memberIndex=0;$memberIndex -lt $Members.Count;$memberIndex++) {
            $expectedMember=$Members[$memberIndex];$loadedMember=$loaded[$memberIndex]
            foreach($identityField in @('member','offset','length','fnv64')) {
                if ([string]$loadedMember.$identityField -cne [string]$expectedMember.$identityField) { throw "Loaded range/content identity mismatch: $identityField" }
            }
            if ($loadedMember.status -ne 'PASS') { throw 'Loaded input failed.' }
        }
    }
    function Check-WorkloadTiming([string]$Prefix,$Spec,$Members,$ExpectedByMember) {
        Check-Loaded $Prefix $Members
        $timedVariants=@($experiment.whole_timed_variants)
        if ($Spec.scope -eq 'blocks') { $timedVariants=@($experiment.block_timed_variants) }
        $models=@(Import-Csv -LiteralPath ($Prefix+'.timed-models.csv'))
        $samples=@(Import-Csv -LiteralPath ($Prefix+'.samples.csv'))
        $summary=@(Import-Csv -LiteralPath ($Prefix+'.timing-summary.csv'))
        $clock=@(Import-Csv -LiteralPath ($Prefix+'.clock.csv'))
        if ($clock.Count -ne 1 -or $clock[0].timer -ne 'QPC' -or [long]$clock[0].frequency -le 0) { throw 'Missing native QPC record.' }
        if ($models.Count -ne ($Members.Count*$timedVariants.Count) -or $summary.Count -ne (2*$timedVariants.Count)) { throw 'Incomplete timing workload.' }
        $modelKeys=@{};$sizeByVariant=@{}
        foreach($modelRow in $models) {
            $modelKey="$($modelRow.member)|$($modelRow.variant)"
            if ($modelKeys.ContainsKey($modelKey) -or $timedVariants -notcontains $modelRow.variant -or $modelRow.status -ne 'PASS') { throw 'Duplicate/invalid timed model.' }
            $modelKeys[$modelKey]=$true
            if (-not $ExpectedByMember.ContainsKey($modelRow.member)) { throw 'Unknown timed member.' }
            $goldRows=@($ExpectedByMember[$modelRow.member] | Where-Object {$_.variant -eq $modelRow.variant})
            if ($goldRows.Count -ne 1) { throw 'No unique audited timed model.' }
            foreach($modelField in @('original_bytes','codec_body_bytes','requested_k','realized_k','emissions','stream_fnv64','map_hex')) {
                if ($modelRow.$modelField -cne $goldRows[0].$modelField) { throw "Timed model differs from audited model: $modelKey / $modelField" }
            }
            if (-not $sizeByVariant.ContainsKey($modelRow.variant)) {$sizeByVariant[$modelRow.variant]=[long]0}
            $sizeByVariant[$modelRow.variant] += [long]$modelRow.codec_body_bytes
        }
        $taskKeys=@{}
        foreach($summaryRow in $summary) {
            $taskKey="$($summaryRow.variant)|$($summaryRow.phase)"
            if ($taskKeys.ContainsKey($taskKey) -or $timedVariants -notcontains $summaryRow.variant -or @('encode','decode') -notcontains $summaryRow.phase) { throw 'Invalid timing task.' }
            $taskKeys[$taskKey]=$true
            if ($summaryRow.profile -ne 'FULL' -or $summaryRow.status -ne 'PASS' -or [int]$summaryRow.repeats -ne 11 -or [int]$summaryRow.members -ne $Members.Count) { throw 'Wrong timing profile or coverage.' }
            if ([long]$summaryRow.original_bytes -ne [long]$Spec.bytes -or [long]$summaryRow.codec_body_bytes -ne $sizeByVariant[$summaryRow.variant]) { throw 'Timing byte denominator mismatch.' }
            $taskSamples=@($samples | Where-Object {$_.variant -eq $summaryRow.variant -and $_.phase -eq $summaryRow.phase})
            $calibration=@($taskSamples | Where-Object {$_.stage -eq 'calibration'})
            $warmup=@($taskSamples | Where-Object {$_.stage -eq 'warmup'})
            $measured=@($taskSamples | Where-Object {$_.stage -eq 'measurement'})
            if ($warmup.Count -ne 3 -or $measured.Count -ne 11 -or $calibration.Count -lt 1 -or $taskSamples.Count -ne (14+$calibration.Count)) { throw 'Missing or extra timing batches.' }
            if (($warmup.round -join ',') -ne '0,1,2' -or ($measured.round -join ',') -ne '0,1,2,3,4,5,6,7,8,9,10') { throw 'Timing replicate IDs changed.' }
            for($pilotIndex=0;$pilotIndex -lt $calibration.Count;$pilotIndex++) {
                $pilot=$calibration[$pilotIndex]
                if ([int]$pilot.round -ne $pilotIndex -or [long]$pilot.iterations -ne [long][Math]::Pow(2,$pilotIndex)) { throw 'Invalid calibration sequence.' }
                if ($pilotIndex -lt $calibration.Count-1 -and (Number $pilot.elapsed_seconds) -ge 0.25) { throw 'Calibration continued after threshold.' }
            }
            if ((Number $calibration[-1].elapsed_seconds) -lt 0.25 -or $calibration[-1].iterations -ne $summaryRow.iterations) { throw 'Missing final calibrated batch.' }
            foreach($sampleRow in $taskSamples) {
                $duration=(Number $sampleRow.elapsed_ticks)/(Number $sampleRow.timer_frequency)
                $operation=$duration/(Number $sampleRow.iterations)
                if ($sampleRow.profile -ne 'FULL' -or $sampleRow.verified -ne '1' -or [long]$sampleRow.elapsed_ticks -le 0 -or $sampleRow.timer_frequency -ne $clock[0].frequency) { throw 'Failed/invalid raw measurement.' }
                if ([long]$sampleRow.original_bytes -ne [long]$Spec.bytes -or [int]$sampleRow.members -ne $Members.Count -or [decimal]$sampleRow.bytes_processed -ne ([decimal]$Spec.bytes*[decimal]$sampleRow.iterations)) { throw 'Raw byte count mismatch.' }
                if (-not (Close-Enough (Number $sampleRow.elapsed_seconds) $duration) -or -not (Close-Enough (Number $sampleRow.seconds_per_operation) $operation)) { throw 'Timer arithmetic mismatch.' }
                if ([int]$sampleRow.below_target -ne [int]($duration -lt .25)) { throw 'Below-target flag mismatch.' }
                if ($sampleRow.stage -ne 'calibration' -and $sampleRow.iterations -ne $summaryRow.iterations) { throw 'Iteration count changed within repeats.' }
            }
            $values=@($measured | ForEach-Object {Number $_.seconds_per_operation})
            foreach($q in @(@('q1_seconds',.25),@('median_seconds',.5),@('q3_seconds',.75))) {
                if (-not (Close-Enough (Number $summaryRow.($q[0])) (Quantile $values $q[1]))) { throw 'Quantile mismatch.' }
            }
            if (-not (Close-Enough (Number $summaryRow.median_MB_per_second) (([double]$Spec.bytes)/(Number $summaryRow.median_seconds)/1000000))) { throw 'Throughput mismatch.' }
        }
        foreach($stageName in @('warmup','measurement')) {
            $rounds=11;if($stageName -eq 'warmup'){$rounds=3}
            for($roundIndex=0;$roundIndex -lt $rounds;$roundIndex++) {
                $roundRows=@($samples | Where-Object {$_.stage -eq $stageName -and [int]$_.round -eq $roundIndex})
                if ($roundRows.Count -ne $summary.Count) { throw 'Round does not contain every task.' }
                $positions=@($roundRows | ForEach-Object {[int]$_.position} | Sort-Object -Unique)
                if (($positions -join ',') -ne ((0..($summary.Count-1)) -join ',')) { throw 'Missing/duplicate task position.' }
            }
        }
        if (@($samples | Where-Object {$timedVariants -notcontains $_.variant -or @('encode','decode') -notcontains $_.phase}).Count -ne 0) { throw 'Unknown sample task.' }
        return $summary
    }
