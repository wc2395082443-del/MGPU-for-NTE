import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path


MAIN_HASH = '8ADEAA052261EA7C89DC6AB3137496B13A5A57D3D7B54F3CE98C776737987E48'
HELPER_HASH = '446FD26095FCB21EEB446D06F67E18485BCEE3D4C432D6B7A3BBB841C9DE6933'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def package(arguments):
    repo = Path(__file__).resolve().parent.parent
    output = arguments.output.resolve()
    if output.exists():
        raise FileExistsError(output)
    if digest(arguments.main) != MAIN_HASH or digest(arguments.helper) != HELPER_HASH:
        raise ValueError('Archived deployed binary identities differ from preview.3')
    baseline = json.loads((arguments.base_package / 'package-manifest.json').read_text(encoding='utf-8-sig'))
    for entry in baseline['Files']:
        relative = Path(entry['Path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Invalid baseline payload path')
        if digest(arguments.base_package / 'payload' / relative) != entry['SHA256']:
            raise ValueError('Baseline payload integrity failure: ' + entry['Path'])
    helper_manifest = json.loads(arguments.helper_manifest.read_text(encoding='utf-8-sig'))
    helper_entries = [entry for entry in helper_manifest['Files'] if entry['Path'] == 'mgpu_official_host.exe']
    if len(helper_entries) != 1 or helper_entries[0]['SHA256'] != HELPER_HASH:
        raise ValueError('Helper manifest identity mismatch')
    output.mkdir(parents=True)
    shutil.copytree(arguments.base_package / 'payload', output / 'payload')
    shutil.copy2(arguments.main, output / 'payload/nvngx.dll_mgpu_bridge.addon64')
    shutil.copy2(arguments.helper, output / 'payload/mgpu-official/mgpu_official_host.exe')
    shutil.copy2(arguments.helper_manifest, output / 'payload/mgpu-official/package-manifest.json')
    manifest = {**baseline, 'Release': 'nte-pipeline-preview.3',
                'GameExeSHA256': 'E72655E227E17796D13EDF00F827DDFEBDE9859B3D758858F0FFC82765D84EFE',
                'NVIDIARuntimeIncluded': False}
    for entry in manifest['Files']:
        entry['SHA256'] = digest(output / 'payload' / entry['Path'])
    (output / 'package-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    shutil.copytree(arguments.base_package / 'licenses', output / 'licenses')
    for name in ('Install.ps1', 'Import-Runtime.ps1', 'runtime-requirements.json', 'Engine.ini.example'):
        shutil.copy2(repo / 'release/nte' / name, output / name)
    shutil.copytree(repo / 'docs/nte', output / 'docs/nte')
    shutil.copy2(repo / 'release/nte/INSTALL-QUICK.zh-CN.md', output / 'INSTALL.zh-CN.md')
    shutil.copy2(repo / 'docs/nte/DEPENDENCIES.md', output / 'DEPENDENCIES.md')
    shutil.copy2(repo / 'release/nte/PIPELINE-NOTES.md', output / 'RELEASE-NOTES.md')
    sums = [f'{digest(path)}  {path.relative_to(output).as_posix()}' for path in sorted(output.rglob('*')) if path.is_file()]
    (output / 'SHA256SUMS.txt').write_text('\n'.join(sums) + '\n', encoding='utf-8')
    archive_path = output.parent / 'MGPU-for-NTE-pipeline-preview.3.zip'
    if archive_path.exists():
        raise FileExistsError(archive_path)
    with zipfile.ZipFile(archive_path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(output.rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(output))
    with zipfile.ZipFile(archive_path) as archive:
        if archive.testzip() is not None:
            raise ValueError('ZIP CRC failure')
        for entry in manifest['Files']:
            actual = hashlib.sha256(archive.read('payload/' + entry['Path'])).hexdigest().upper()
            if actual != entry['SHA256']:
                raise ValueError('ZIP payload hash mismatch')
        forbidden = ('nvngx_dlssg.dll', 'nvngx_dlssnr.dll', 'sl.interposer.dll', 'sl.common.dll',
                     'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'HTGame.exe')
        if any(Path(entry.filename).name in forbidden for entry in archive.infolist()):
            raise ValueError('Runtime/model/game binary in package')
    print(json.dumps({'archive': str(archive_path), 'sha256': digest(archive_path), 'payload': manifest['Files']}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--main', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--helper-manifest', type=Path, required=True)
    parser.add_argument('--base-package', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    package(parser.parse_args())
