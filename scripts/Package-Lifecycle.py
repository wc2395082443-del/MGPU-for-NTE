import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile


MAIN_HASH='B09CA7B12A3B23069FFB35DDB9F08D8684239DBAFDFEBB486E78EEAFA7922BF0'
HELPER_HASH='E172D87654241079991C3D8766BDCE66B7B763235720703EAA1F3B1DD3B0A767'
GAME_HASH='64ED3CAE0B5FC61F3B56CDD933B4807CB23F0EC7913590DB60D1A8642DE2DFEC'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def package(args):
    repo=Path(__file__).resolve().parent.parent
    output=args.output.resolve()
    if output.exists():
        raise FileExistsError(output)
    if digest(args.accepted/'mgpu_bridge.addon64')!=MAIN_HASH or digest(args.accepted/'mgpu-official/mgpu_official_host.exe')!=HELPER_HASH:
        raise ValueError('Accepted B09C/E172 artifact hash mismatch')
    output.mkdir(parents=True)
    payload=output/'payload'
    files=[('nvngx.dll_mgpu_bridge.addon64',args.accepted/'mgpu_bridge.addon64',False),
           ('mgpu-official/mgpu_official_host.exe',args.accepted/'mgpu-official/mgpu_official_host.exe',False),
           ('mgpu-official/package-manifest.json',args.accepted/'mgpu-official/package-manifest.json',False),
           ('mgpu-official/NVIDIA-Streamline-license.txt',args.accepted/'mgpu-official/NVIDIA-Streamline-license.txt',False),
           ('reshade-shaders/Shaders/mgpu_depth_tap.fx',repo/'assets/reshade-shaders/Shaders/mgpu_depth_tap.fx',False)]
    files.extend((name,repo/'release/nte'/name,True) for name in ['mgpu.ini','gpu1.ini','ReShade2.ini'])
    entries=[]
    for relative,source,configuration in files:
        destination=payload/relative
        destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(source,destination)
        entries.append(dict(Path=relative,SHA256=digest(destination),Configuration=configuration))
    manifest=dict(Release='nte-lifecycle-preview.4',GameExeSHA256=GAME_HASH,NVIDIARuntimeIncluded=False,
        DiagnosticBuild=True,Files=entries)
    (output/'package-manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    for name in ['Install.ps1','Import-Runtime.ps1','runtime-requirements.json','Engine.ini.example']:
        shutil.copy2(repo/'release/nte'/name,output/name)
    shutil.copy2(repo/'release/nte/LIFECYCLE-NOTES.md',output/'RELEASE-NOTES.md')
    shutil.copy2(repo/'release/nte/INSTALL-QUICK.zh-CN.md',output/'INSTALL.zh-CN.md')
    shutil.copytree(repo/'docs/nte',output/'docs/nte')
    licenses=output/'licenses'
    licenses.mkdir()
    shutil.copy2(repo/'LICENSE',licenses/'LICENSE')
    shutil.copy2(repo/'ext/streamline/LICENSE.txt',licenses/'Streamline-headers-LICENSE.txt')
    shutil.copy2(repo/'THIRD_PARTY.md',licenses/'THIRD_PARTY.md')
    shutil.copy2(repo/'ACKNOWLEDGEMENTS.md',licenses/'ACKNOWLEDGEMENTS.md')
    if (repo/'ext/reshade/LICENSE.md').exists():
        shutil.copy2(repo/'ext/reshade/LICENSE.md',licenses/'ReShade-LICENSE.md')
    sums=[f'{digest(path)}  {path.relative_to(output).as_posix()}' for path in sorted(output.rglob('*')) if path.is_file()]
    (output/'SHA256SUMS.txt').write_text('\n'.join(sums)+'\n',encoding='utf-8')
    archive=output.parent/'MGPU-for-NTE-lifecycle-preview.4.zip'
    if archive.exists():
        raise FileExistsError(archive)
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as zipped:
        for path in sorted(output.rglob('*')):
            if path.is_file():
                zipped.write(path,path.relative_to(output))
    with zipfile.ZipFile(archive) as zipped:
        if zipped.testzip():
            raise ValueError('ZIP CRC failed')
        forbidden={'nvngx_dlssg.dll','nvngx_dlssnr.dll','sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll','HTGame.exe'}
        if any(Path(item.filename).name in forbidden for item in zipped.infolist()):
            raise ValueError('Third-party runtime, model or game binary included')
        for entry in entries:
            if hashlib.sha256(zipped.read('payload/'+entry['Path'])).hexdigest().upper()!=entry['SHA256']:
                raise ValueError('ZIP payload hash mismatch')
    print(json.dumps(dict(archive=str(archive),SHA256=digest(archive),files=entries),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--accepted',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    package(parser.parse_args())
