"""Build original PS2 programs, then verify their LLVM AOT and optional reference results."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
import uuid

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[1]
MAGIC = 0x41503254
RESULT_WORDS = 21


def command(args, log, env, accepted=(0,), timeout=180):
    started = time.monotonic()
    result = subprocess.run([str(arg) for arg in args], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    log.write_bytes(result.stdout)
    if result.returncode not in accepted:
        raise RuntimeError(f"Command returned {result.returncode}; see {log}")
    return result, time.monotonic() - started


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def observations(raw, case):
    if len(raw) != RESULT_WORDS * 4:
        raise ValueError("Truncated test result")
    words = struct.unpack('<21I', raw)
    header = [MAGIC, 1, case['id'], 1, len(case['values'])]
    if list(words[:5]) != header:
        raise ValueError(f"Incomplete or wrong test result: {list(words[:5])}; expected {header}")
    values = list(words[5:5 + len(case['values'])])
    if values != case['values']:
        raise ValueError(f"Behavior mismatch: {values}; expected {case['values']}")
    return values


def receive(sock, size):
    result = bytearray()
    while len(result) < size:
        block = sock.recv(size - len(result))
        if not block:
            raise ConnectionError("Reference closed PINE connection")
        result.extend(block)
    return bytes(result)


def pine(sock, payload):
    sock.sendall(struct.pack('<I', len(payload) + 4) + payload)
    size, = struct.unpack('<I', receive(sock, 4))
    if size < 5 or size > 4096:
        raise ValueError("Invalid PINE reply length")
    reply = receive(sock, size - 4)
    if reply[0] != 0:
        raise RuntimeError("Reference rejected PINE request")
    return reply[1:]


def reference(exe, bios, elf, address, case, directory, env):
    directory.mkdir()
    # PCSX2 2.8 appends its application directory to -datapath on Windows.
    configuration = directory / 'PCSX2' / 'inis'
    configuration.mkdir(parents=True)
    # Each owned process has its own settings, writable data and loopback port.
    # The BIOS remains an external, user-supplied reference-only input.
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    settings = (
        '[UI]\nSettingsVersion=1\nSetupWizardIncomplete=false\nConfirmShutdown=false\n'
        'PauseOnFocusLoss=false\n[Folders]\nBios=' + str(bios.parent) + '\n'
        '[Filenames]\nBIOS=' + bios.name + '\n'
        '[EmuCore]\nEnablePINE=true\nPINESlot=' + str(port) + '\n'
        '[EmuCore/GS]\nRenderer=11\n')
    (configuration / 'PCSX2.ini').write_text(settings, encoding='utf-8')
    with (directory / 'process.log').open('wb') as output:
        process = subprocess.Popen([str(exe), '-datapath', str(directory), '-nogui', '-batch',
                                    '-elf', str(elf), '-logfile', str(directory / 'reference.log')],
                                   env=env, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 45
            last_error = 'reference did not publish completion'
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"Reference exited with {process.returncode}; see {directory}")
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=1) as client:
                        client.settimeout(2)
                        raw = pine(client, b''.join(struct.pack('<BI', 2, address + n * 4)
                                                  for n in range(RESULT_WORDS)))
                        if len(raw) == RESULT_WORDS * 4 and struct.unpack_from('<I', raw, 12)[0] == 1:
                            values = observations(raw, case)
                            (directory / 'result.bin').write_bytes(raw)
                            version = pine(client, bytes([8]))
                            if len(version) < 4 or struct.unpack_from('<I', version)[0] != len(version) - 4:
                                raise ValueError('Invalid reference version reply')
                            identity = dict(executable_sha256=digest(exe),
                                            version=version[4:].rstrip(b'\0').decode('utf-8'))
                            (directory / 'identity.json').write_text(json.dumps(identity, indent=2))
                            return values
                except (ConnectionError, TimeoutError, OSError, RuntimeError) as error:
                    last_error = str(error)
                time.sleep(0.05)
            raise RuntimeError('Reference timed out: ' + last_error)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def main():
    catalog = json.loads((SOURCE / 'cases.json').read_text())
    if catalog['format'] != 'anyps2-source-tests-v1':
        raise ValueError('Unsupported test catalog')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ps2dev', type=Path, required=True)
    parser.add_argument('--host-runtime', type=Path, help='Optional MinGW32 host DLL directory')
    parser.add_argument('--converter', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--generator', default='Visual Studio 18 2026')
    parser.add_argument('--case', action='append', choices=[case['name'] for case in catalog['cases']],
                        help='Run selected families; the default runs every family')
    parser.add_argument('--reference-exe', type=Path)
    parser.add_argument('--bios', type=Path)
    args = parser.parse_args()
    if bool(args.reference_exe) != bool(args.bios):
        parser.error('--reference-exe and --bios must be supplied together')
    env = os.environ.copy()
    if args.host_runtime:
        env['PATH'] = str(args.host_runtime.resolve()) + os.pathsep + env.get('PATH', '')
    run = args.output_root.resolve() / uuid.uuid4().hex[:12]
    run.mkdir(parents=True)
    print('Evidence:', run, flush=True)
    ps2dev = args.ps2dev.resolve()
    converter = args.converter.resolve()
    platform = ['-A', 'x64'] if 'Visual Studio' in args.generator else []
    inputs = run / 'ps2'
    command(['cmake', '-S', SOURCE, '-B', inputs, '-G', args.generator, *platform,
             '-DPS2DEV=' + str(ps2dev)], run / 'ps2-configure.log', env)
    command(['cmake', '--build', inputs, '--config', 'Release', '--parallel', '2'],
            run / 'ps2-build.log', env)
    report = dict(format='anyps2-source-test-results-v1', compiler=(inputs / 'compiler-version.txt').read_text(),
                  converter_sha256=digest(converter),
                  sources={path.name: digest(path) for path in SOURCE.iterdir() if path.is_file()}, cases=[])
    native_source, native_build = run / 'native-project', run / 'native-build'
    native_source.mkdir()
    failed = False
    for case in catalog['cases']:
        if args.case and case['name'] not in args.case:
            continue
        for opt in (0, 2):
            name = f"{case['name']}-O{opt}"
            directory = run / name
            directory.mkdir()
            elf = inputs / (name + '.elf')
            row = dict(name=name, ps2_built=True, elf_sha256=digest(elf), verification='not-run',
                       generation='not-run', compilation='not-run', execution='not-run', reference='not-run')
            report['cases'].append(row)
            stage = 'verification'
            try:
                result, _ = command([ps2dev / 'ee/bin/mips64r5900el-ps2-elf-nm.exe', '-P', elf],
                                    directory / 'symbols.txt', env)
                symbols = {}
                for line in result.stdout.decode().splitlines():
                    fields = line.split()
                    if len(fields) >= 3:
                        symbols[fields[0]] = int(fields[2], 16)
                address = symbols['anyps2_test_result']
                done = symbols['anyps2_test_done']
                row['result_address'] = address
                command([ps2dev / 'ee/bin/mips64r5900el-ps2-elf-objdump.exe', '-dr', elf],
                        directory / 'disassembly.txt', env)
                result, _ = command([converter, elf, directory / 'verify', '--verify-only'],
                                    directory / 'verify.log', env, accepted=(0, 2))
                row['verification'] = 'complete' if result.returncode == 0 else 'incomplete'
                summary = (directory / 'verify/flow-verification.txt').read_text()
                row['coverage'] = dict(line.split('=', 1) for line in summary.splitlines() if '=' in line)
                if args.reference_exe:
                    try:
                        row['reference_values'] = reference(args.reference_exe.resolve(), args.bios.resolve(),
                                                           elf, address, case, directory / 'reference', env)
                        row['reference'] = 'passed'
                    except (RuntimeError, ValueError, OSError) as error:
                        row['reference'] = 'failed'
                        row['reference_error'] = str(error)
                        failed = True
                project = directory / 'converted'
                stage = 'generation'
                _, row['generation_seconds'] = command([converter, elf, project], directory / 'generate.log', env)
                row['generation'] = 'passed'
                stage = 'compilation'
                for source in project.iterdir():
                    target = native_source / source.name
                    if source.is_file() and (not target.exists() or digest(source) != digest(target)):
                        shutil.copyfile(source, target)
                command(['cmake', '-S', native_source, '-B', native_build, '-G', args.generator,
                         *platform, '-DANYPS2_ROOT=' + str(ROOT)], directory / 'configure.log', env)
                _, row['compilation_seconds'] = command(
                    ['cmake', '--build', native_build, '--config', 'Release', '--target', 'ps2-native', '--parallel', '2'],
                    directory / 'compile.log', env)
                row['compilation'] = 'passed'
                exe = native_build / 'Release/ps2-native.exe'
                if 'dependency audit passed' not in Path(str(exe) + '.audit.txt').read_text():
                    raise ValueError('Native dependency audit missing or failed')
                row['native_sha256'] = digest(exe)
                stage = 'execution'
                capture = directory / 'native-stop'
                result, _ = command([exe, native_source / 'program.data', '200000', '--headless', '--capture-stop', capture],
                                    directory / 'native.log', env, accepted=(3, 4), timeout=15)
                cpu = dict(line.split('=', 1) for line in (capture / 'cpu.txt').read_text().splitlines())
                row['native_stop'] = dict(reason=int(cpu['stop']), pc=f"0x{int(cpu['pc']):08x}",
                                          executed=int(cpu['executed']))
                if result.returncode != 4 or int(cpu['pc']) not in (done, done + 4):
                    raise ValueError('Native execution stopped before the completion loop; see native.log')
                with (capture / 'ram.bin').open('rb') as memory:
                    memory.seek(address)
                    raw = memory.read(RESULT_WORDS * 4)
                row['native_values'] = observations(raw, case)
                if row['reference'] == 'passed' and row['native_values'] != row['reference_values']:
                    raise ValueError('Native/reference results differ')
                row['execution'] = 'passed'
            except (RuntimeError, ValueError, OSError, KeyError, subprocess.TimeoutExpired) as error:
                row[stage] = 'failed'
                row['error'] = str(error)
                failed = True
            (run / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            print(name, row.get('error', row.get('reference_error', 'PASS')), flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
