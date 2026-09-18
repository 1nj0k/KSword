# Real System.Management embedded outputs and schema; no BCD method executes here.
#requires -Version 5.1
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'AmdLabBoot.psm1') -Force
& (Get-Module AmdLabBoot) {
    $checks = 0
    function Assert($Condition, $Name) {
        if (-not $Condition) { throw "FAILED: $Name" }
        $script:bindingChecks++
    }
    $script:bindingChecks = 0
    $class = [wmiclass]'root\WMI:BcdStore'
    $envelope = $class.Methods['OpenStore'].OutParameters.Clone()
    $store = $class.CreateInstance()
    $store.FilePath = ''
    $envelope.Store = $store
    $embedded = $envelope.Store
    Assert ($embedded.GetType() -eq [System.Management.ManagementBaseObject]) 'reproduce real embedded output type'
    Assert ($null -eq $embedded.PSObject.Methods['OpenObject']) 'unbound data has no OpenObject'
    $bound = ConvertTo-AmdBcdInstance $embedded
    Assert ($bound -is [System.Management.ManagementObject]) 'store rebound to invocable type'
    Assert ($bound.PSBase.Path.RelativePath -eq 'BcdStore.FilePath=""') 'system store key preserved'
    Assert ($bound.PSBase.Scope.Path.NamespacePath -eq 'root\WMI') 'provider namespace preserved'
    # Object outputs require the same conversion; no OpenObject or mutation is invoked.
    $envelope = $class.Methods['OpenObject'].OutParameters.Clone()
    $object = ([wmiclass]'root\WMI:BcdObject').CreateInstance()
    $object.StoreFilePath = ''
    $object.Id = '{11111111-1111-1111-1111-111111111111}'
    $envelope.Object = $object
    $bound = ConvertTo-AmdBcdInstance $envelope.Object
    Assert ($bound -is [System.Management.ManagementObject]) 'BCD object rebound'
    Assert ($bound.PSBase.Path.RelativePath -eq 'BcdObject.Id="{11111111-1111-1111-1111-111111111111}",StoreFilePath=""') 'object keys preserved'
    $store.FilePath = 'C:\test\sample.bcd'
    $envelope = $class.Methods['OpenStore'].OutParameters.Clone()
    $envelope.Store = $store
    $bound = ConvertTo-AmdBcdInstance $envelope.Store
    Assert ($bound.PSBase.Path.RelativePath -eq 'BcdStore.FilePath="C:\\test\\sample.bcd"') 'backslashes escaped in WMI key'
    "BCD_BINDING_CHECKS=$script:bindingChecks RESULT=PASS (schema and binding only; no BCD methods executed)"
}
