# Generates src/items_data.inc from data/item_ids.json.
# Run from the repo root:  powershell -ExecutionPolicy Bypass -File tools\gen_items.ps1
#
# The item list comes from veeenu/darksoulsiii-practice-tool (AGPL-3.0).
# Categories are translated to Portuguese and armor pieces are grouped into sets.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$json = Get-Content (Join-Path $root 'data\item_ids.json') -Raw -Encoding UTF8 | ConvertFrom-Json

# Top-level node / sub node  ->  category, group
$categoryMap = @{
    'Melee weapons'  = 'Armas'
    'Shields'        = 'Escudos'
    'Ranged weapons' = 'Arcos e bestas'
    'Arrows'         = 'Flechas e virotes'
    'Catalysts'      = 'Catalisadores'
    'Rings'          = 'Anéis'
}
$itemsSubMap = @{
    'Consumables'            = 'Consumíveis'
    'Souls'                  = 'Almas'
    'Boss souls'             = 'Almas de chefe'
    'Upgrades and infusions' = 'Melhorias'
    'Key items'              = 'Itens-chave'
    'Coals and ashes'        = 'Brasas e cinzas'
    'Books'                  = 'Livros e pergaminhos'
    'Gestures'               = 'Gestos'
    'Spells'                 = 'Magias'
}
$groupMap = @{
    'Daggers' = 'Adagas'; 'Straight swords' = 'Espadas retas'; 'Greatswords' = 'Espadões'
    'Ultra Greatswords' = 'Ultra espadões'; 'Curved swords' = 'Espadas curvas'
    'Curved Greatswords' = 'Espadões curvos'; 'Katanas' = 'Katanas'
    'Thrusting swords' = 'Espadas de estocada'; 'Axes' = 'Machados'; 'Greataxes' = 'Machados grandes'
    'Hammers' = 'Martelos'; 'Greathammers' = 'Martelos grandes'; 'Spears and Pikes' = 'Lanças e piques'
    'Halberds' = 'Alabardas'; 'Reapers' = 'Foices'; 'Fists' = 'Punhos e garras'; 'Whips' = 'Chicotes'
    'Misc' = 'Outras'; 'Bows' = 'Arcos'; 'Greatbows' = 'Arcos grandes'; 'Crossbows' = 'Bestas'
    'Bow arrows' = 'Flechas'; 'Greatbow arrows' = 'Flechas grandes'; 'Crossbow bolts' = 'Virotes'
    'Staves' = 'Cajados'; 'Flames' = 'Chamas de piromancia'; 'Talismans' = 'Talismãs'; 'Chimes' = 'Sinos'
    'Small shields' = 'Escudos pequenos'; 'Standard shields' = 'Escudos médios'; 'Greatshields' = 'Escudos grandes'
    'Sorceries' = 'Feitiçarias'; 'Pyromancies' = 'Piromancias'; 'Miracles' = 'Milagres'
}
$categoryOrder = @('Armas', 'Escudos', 'Arcos e bestas', 'Flechas e virotes', 'Catalisadores', 'Magias',
    'Sets de armadura', 'Anéis', 'Consumíveis', 'Melhorias', 'Almas', 'Almas de chefe',
    'Brasas e cinzas', 'Itens-chave', 'Livros e pergaminhos', 'Gestos')
# 0 = weapon (upgrade/infusion), 1 = ammo/stackable, 2 = armor set, 3 = single item
$categoryKind = @{
    'Armas' = 0; 'Escudos' = 0; 'Arcos e bestas' = 0; 'Catalisadores' = 0
    'Flechas e virotes' = 1; 'Consumíveis' = 1; 'Melhorias' = 1; 'Almas' = 1; 'Almas de chefe' = 1; 'Brasas e cinzas' = 3
    'Sets de armadura' = 2; 'Anéis' = 3; 'Magias' = 3; 'Itens-chave' = 3; 'Livros e pergaminhos' = 3; 'Gestos' = 3
}

function Leaves($node) {
    if ($node.PSObject.Properties.Name -contains 'id') { return , $node }
    $out = @()
    foreach ($c in $node.children) { $out += Leaves $c }
    return $out
}

$rows = New-Object System.Collections.Generic.List[object]
$armor = New-Object System.Collections.Generic.List[object]

foreach ($top in $json) {
    if ($top.node -eq 'Armor') { foreach ($l in (Leaves $top)) { $armor.Add($l) }; continue }
    if ($top.node -eq 'Items') {
        foreach ($sub in $top.children) {
            $cat = $itemsSubMap[$sub.node]
            if (-not $cat) { throw "Unmapped Items sub node: $($sub.node)" }
            $hasGroups = $sub.children | Where-Object { $_.PSObject.Properties.Name -contains 'node' }
            if ($hasGroups) {
                foreach ($g in $sub.children) {
                    $grp = $groupMap[$g.node]; if (-not $grp) { $grp = $g.node }
                    foreach ($l in (Leaves $g)) { $rows.Add([pscustomobject]@{ cat = $cat; grp = $grp; name = $l.desc; id = $l.id }) }
                }
            } else {
                foreach ($l in (Leaves $sub)) { $rows.Add([pscustomobject]@{ cat = $cat; grp = $cat; name = $l.desc; id = $l.id }) }
            }
        }
        continue
    }
    $cat = $categoryMap[$top.node]
    if (-not $cat) { throw "Unmapped top node: $($top.node)" }
    $hasGroups = $top.children | Where-Object { $_.PSObject.Properties.Name -contains 'node' }
    if ($hasGroups) {
        foreach ($g in $top.children) {
            $grp = $groupMap[$g.node]; if (-not $grp) { $grp = $g.node }
            foreach ($l in (Leaves $g)) { $rows.Add([pscustomobject]@{ cat = $cat; grp = $grp; name = $l.desc; id = $l.id }) }
        }
    } else {
        foreach ($l in (Leaves $top)) { $rows.Add([pscustomobject]@{ cat = $cat; grp = $cat; name = $l.desc; id = $l.id }) }
    }
}

# Armor: pieces of one set share (paramId / 10000). Helm = +0, chest = +1000, hands = +2000, legs = +3000.
$fillers = @('of', 'the')
function SharedWords([string[]]$names, [bool]$fromEnd) {
    $words = $names | ForEach-Object { $w = $_ -split ' '; if ($fromEnd) { [array]::Reverse($w) }; , $w }
    $shared = @()
    for ($i = 0; ; $i++) {
        $w = $null
        foreach ($ws in $words) {
            if ($i -ge $ws.Count) { $w = $null; break }
            if ($null -eq $w) { $w = $ws[$i] } elseif ($w -ne $ws[$i]) { $w = $null; break }
        }
        if ($null -eq $w) { break }
        $shared += $w
    }
    if ($fromEnd) { [array]::Reverse($shared) }
    return , $shared
}
function SetName([string[]]$names) {
    if ($names.Count -eq 1) { return $names[0] }
    # "Alva Helm", "Alva Armor"... -> "Alva"
    $prefix = SharedWords $names $false
    while ($prefix.Count -gt 0 -and $fillers -contains $prefix[-1]) { $prefix = @($prefix | Select-Object -SkipLast 1) }
    if ($prefix.Count -gt 0) { return ($prefix -join ' ') }
    # "Helm of Thorns", "Armor of Thorns"... -> "Thorns"
    $suffix = SharedWords $names $true
    while ($suffix.Count -gt 0 -and $fillers -contains $suffix[0]) { $suffix = @($suffix | Select-Object -Skip 1) }
    if ($suffix.Count -gt 0) { return ($suffix -join ' ') }
    # Nothing shared ("Black Hand Hat" + "Assassin Gloves"): name it after the first piece.
    return $names[0]
}
$sets = $armor | Group-Object { [math]::Floor(([Convert]::ToUInt32($_.id, 16) -band 0x0FFFFFFF) / 10000) }
$usedNames = @{}
$setRows = foreach ($s in $sets) {
    $pieces = $s.Group | Sort-Object { [Convert]::ToUInt32($_.id, 16) }
    $setName = SetName ([string[]]($pieces | ForEach-Object desc))
    if ($usedNames.ContainsKey($setName)) { $setName = "$setName ($($pieces[0].desc))" }
    $usedNames[$setName] = $true
    foreach ($p in $pieces) { [pscustomobject]@{ cat = 'Sets de armadura'; grp = $setName; name = $p.desc; id = $p.id } }
}
$setRows = $setRows | Sort-Object grp, { [Convert]::ToUInt32($_.id, 16) }
foreach ($r in $setRows) { $rows.Add($r) }

function Esc([string]$s) { return $s.Replace('\', '\\').Replace('"', '\"') }

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('// AUTOGENERATED by tools/gen_items.ps1 from data/item_ids.json - do not edit by hand.')
[void]$sb.AppendLine('// Item list: veeenu/darksoulsiii-practice-tool (AGPL-3.0).')
[void]$sb.AppendLine('// Format: CATEGORY(name, kind) followed by ITEM(group, name, id) rows.')
foreach ($cat in $categoryOrder) {
    $catRows = @($rows | Where-Object { $_.cat -eq $cat })
    if ($catRows.Count -eq 0) { throw "Empty category $cat" }
    [void]$sb.AppendLine("CATEGORY(""$(Esc $cat)"", $($categoryKind[$cat]))")
    foreach ($r in $catRows) {
        [void]$sb.AppendLine("ITEM(""$(Esc $r.grp)"", ""$(Esc $r.name)"", 0x$($r.id.ToUpper()))")
    }
}
$missing = $rows | Where-Object { $categoryOrder -notcontains $_.cat } | Select-Object -ExpandProperty cat -Unique
if ($missing) { throw "Categories missing from order: $($missing -join ', ')" }

$out = Join-Path $root 'src\items_data.inc'
[IO.File]::WriteAllText($out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Wrote $out ($($rows.Count) items, $(@($sets).Count) armor sets)"
