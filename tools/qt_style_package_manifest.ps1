function Get-ObriiQtStylePackageFiles([string]$Runtime) {
    $extra = @('styles/qcleanlooksstyle.dll', 'styles/qmotifstyle.dll', 'styles/qplastiquestyle.dll') |
        Where-Object { Test-Path -LiteralPath (Join-Path $Runtime $_) -PathType Leaf }
    if ($extra) {
        $notices = @('licenses/qt-styleplugins/README.md', 'licenses/qt-styleplugins/LICENSE.LGPL-2.1.txt',
            'licenses/qt-styleplugins/LGPL_EXCEPTION.txt', 'licenses/qt-styleplugins/qtstyleplugins-335dbece-source.tar.gz')
        foreach ($notice in $notices) {
            if (-not (Test-Path -LiteralPath (Join-Path $Runtime $notice) -PathType Leaf)) {
                throw "Classic Qt style plugin source or notice missing: $notice"
            }
        }
        $extra
        $notices
    }
    $vista = 'styles/qwindowsvistastyle.dll'
    if (Test-Path -LiteralPath (Join-Path $Runtime $vista) -PathType Leaf) { $vista }
}
