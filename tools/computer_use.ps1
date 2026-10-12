# Windows PowerShell 5.1+, system UI Automation only. No global input.
param([string[]]$AllowApp, [switch]$Check, [switch]$DryRun)
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if ($Check) { @{platform='windows'; background=$true; images=$false} | ConvertTo-Json -Compress; exit }
if (!$AllowApp) { throw 'Explicit -AllowApp process-name allowlist required' }
$script:elements = @()
$script:stateId = ''
$script:appId = ''
function Observe($app) {
    $script:elements = @()
    $script:stateId = [guid]::NewGuid().ToString()
    $script:appId = $app
    $rows = [Collections.Generic.List[string]]::new()
    if (!$DryRun -and $AllowApp -contains $app) {
        $pids = @(Get-Process -Name $app -ErrorAction SilentlyContinue | ForEach-Object Id)
        $root = [System.Windows.Automation.AutomationElement]::RootElement
        $windows = $root.FindAll([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)
        foreach ($window in $windows) {
            if ($pids -notcontains $window.Current.ProcessId) { continue }
            $nodes = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($node in $nodes) {
                if ($script:elements.Count -ge 160) { break }
                # Password values are never observed.
                if ($node.Current.IsPassword) { continue }
                $index = $script:elements.Count
                $script:elements += $node
                $name = $node.Current.Name
                if ($name.Length -gt 600) { $name = $name.Substring(0,600) }
                $value = ''; $pattern = $null
                if ($node.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$pattern)) {
                    $value = $pattern.Current.Value
                    if ($value.Length -gt 600) { $value = $value.Substring(0,600) }
                }
                $rows.Add("$index $($node.Current.ControlType.ProgrammaticName) | $name | $value")
            }
        }
    }
    return @{state_id=$script:stateId; app=$app; controls=($rows -join "`n"); dry_run=[bool]$DryRun; background=$true}
}
function Batch($a) {
    $done = 0; $failure = ''
    try {
        if ($AllowApp -notcontains $a.app) { throw 'App is not allowlisted' }
        if ($a.observation -and $a.observation -notin @('auto','text')) { throw 'Only text/auto observations are supported' }
        if ($null -eq $a.actions -or $a.actions -isnot [array] -or $a.actions.Count -gt 12) { throw 'Use an array of up to 12 actions' }
        foreach ($action in $a.actions) {
            if ($action.type -notin @('press_element','set_value')) { throw 'Global input and activation are disabled' }
            if ($null -eq $action.element -or $action.element -is [string] -or $action.element -lt 0 -or [double]$action.element -ne [int]$action.element) { throw 'Nonnegative integer element required' }
            if ($action.type -eq 'set_value' -and ($action.text -isnot [string] -or $action.text.Length -gt 4000)) { throw 'set_value requires text of at most 4000 characters' }
        }
        if ($a.actions.Count -and ($a.state_id -ne $script:stateId -or $a.app -ne $script:appId)) { throw 'Stale element reference; observe again' }
        foreach ($action in $a.actions) {
            if (!$DryRun) {
                if ($action.element -ge $script:elements.Count) { throw 'Element out of bounds' }
                $node = $script:elements[$action.element]
                if ($node.Current.IsPassword) { throw 'Password control is unavailable' }
                if ($action.type -eq 'set_value') {
                    $pattern = $node.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
                    $pattern.SetValue($action.text)
                } else {
                    $pattern = $node.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
                    $pattern.Invoke()
                }
            }
            $done++
        }
    } catch { $failure = $_.Exception.Message }
    $state = Observe $a.app
    return @{isError=([bool]$failure); content=@(@{type='text'; text=(@{completed_actions=$done; error=$failure; state=$state} | ConvertTo-Json -Depth 12 -Compress)})}
}
$tool = @{name='computer_batch'; description='Background Windows UI Automation. app is an allowlisted process name. Observe with actions=[]; then use state_id and element for press_element (InvokePattern) or set_value(text, replaces value). No global input, focus or activation. Unsupported controls fail without fallback. Inspect actual resulting state; applications can activate their own windows.'; inputSchema=@{type='object'; properties=@{app=@{type='string'}; actions=@{type='array'; maxItems=12; items=@{type='object'}}; observation=@{type='string'; enum=@('auto','text')}; state_id=@{type='string'}}; required=@('app','actions'); additionalProperties=$false}}
while ($null -ne ($line = [Console]::ReadLine())) {
    try {
        $r = $line | ConvertFrom-Json
        if ($null -eq $r.id) { continue }
        $result = @{}
        switch ($r.method) {
            'initialize' { $result=@{protocolVersion='2025-11-25'; capabilities=@{tools=@{}}; serverInfo=@{name='native-computer-use'; version='0.2'}} }
            'tools/list' { $result=@{tools=@($tool)} }
            'tools/call' {
                if ($r.params.name -ne 'computer_batch') { throw 'Unknown tool' }
                $result=Batch $r.params.arguments
            }
        }
        [Console]::WriteLine((@{jsonrpc='2.0'; id=$r.id; result=$result} | ConvertTo-Json -Depth 20 -Compress))
    } catch {
        if ($null -ne $r.id) { [Console]::WriteLine((@{jsonrpc='2.0'; id=$r.id; error=@{code=-32602; message=$_.Exception.Message}} | ConvertTo-Json -Compress)) }
    }
}
