#!/usr/bin/env python3

""" Monitor the crash case generated by AFL """

# Libraries
import argparse
import io
import os
import selectors
import sys
import shutil
import subprocess
from timeit import default_timer as timer
from typing import Any, Callable, TextIO
from watchdog.events import FileSystemEventHandler, FileCreatedEvent, \
    FileSystemEvent
from watchdog.observers import Observer


Runner = Callable[[str, str], tuple[str, float]]


class CrashMonitor(FileSystemEventHandler):
    """ Monitor a crash directory """

    def __init__(
        self, output_dir: str, run_official: Runner, run_custom: Runner
    ) -> None:
        """ Constructor """
        super().__init__()
        assert os.path.isdir(output_dir), output_dir
        self.output_dir = output_dir
        self.run_official = run_official
        self.run_custom = run_custom

    def on_created(self, event: FileSystemEvent) -> None:
        """ Event representing file/dir creation """
        if not isinstance(event, FileCreatedEvent):
            return
        self.process(event.src_path)

    def process(self, crash_file: str) -> None:
        """ Process a crash file """
        assert os.path.isfile(crash_file), crash_file
        print(f"\n===> Detected new crash case: {crash_file}")
        crash_case = os.path.join(self.output_dir, "crash_case")
        crash_reduce = os.path.join(self.output_dir, "crash_reduce")
        crash_official = os.path.join(self.output_dir, "crash_official")
        shutil.copyfile(crash_file, crash_case)
        crash_case_size = os.path.getsize(crash_case)
        print(f"===> Copied {crash_case_size} bytes to {crash_case}")

        print("===> Running official afl-tmin...")
        official_output, official_time = self.run_official(
            crash_case, crash_official)
        print("===> Official afl-tmin statistics:")
        crash_official_size = os.path.getsize(crash_official)
        official_percent = (1 - crash_official_size / crash_case_size) * 100
        print(f"===> Final file size: {crash_official_size} " +
              f"({official_percent:.1f}% reduction)")
        print(f"===> Elapsed time: {official_time:.2f} seconds")
        print(official_output)

        if os.path.exists(crash_case):
            os.remove(crash_case)
        if os.path.exists(crash_reduce):
            os.remove(crash_reduce)
        if os.path.exists(crash_official):
            os.remove(crash_official)


def run_afl_tmin(
    afl_tmin_binary: str, test_case_dir: str, execute_line: list[str],
    add_d_option: bool = False
) -> Runner:
    """ Run afl-tmin, return the output """
    def runner(crash_case: str, crash_reduce: str) -> tuple[str, float]:
        # Create process
        start = timer()
        process = subprocess.Popen(
            " ".join(
                [afl_tmin_binary, "-i", crash_case, "-o", crash_reduce] + (
                    ["-d", test_case_dir] if add_d_option else []
                ) + ["--"] + execute_line
            ), bufsize=1, universal_newlines=True, shell=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )

        # Create callback function for process output
        buf = io.StringIO()

        def handle_output(stream: TextIO, _: Any) -> None:
            # Because the process' output is line buffered, there's only ever
            # one line to read when this function is called
            line = stream.readline()
            buf.write(line)
            sys.stdout.write(line)

        # Register callback for an "available for read" event
        selector = selectors.DefaultSelector()
        selector.register(
            process.stdout,  # type: ignore
            selectors.EVENT_READ, handle_output
        )

        # Loop until subprocess is terminated
        while process.poll() is None:
            # Wait for events and handle them with their registered callbacks
            events = selector.select()
            for key, mask in events:
                callback = key.data
                callback(key.fileobj, mask)

        # Get process return code
        return_code = process.wait()
        selector.close()
        end = timer()

        if return_code != 0:
            print(f"Warning: the process returned {return_code}!")

        # Store buffered output
        output = buf.getvalue()
        buf.close()

        return output, end - start
    return runner


def main() -> None:
    """ Main function """
    parser = argparse.ArgumentParser()
    parser.add_argument("-i", "--input-dir",
                        help="AFL crash directory to monitor")
    parser.add_argument("-o", "--output-dir", help="Output directory")
    parser.add_argument("-d", "--test-case-dir", help="Testcase directory")
    parser.add_argument("--official", help="Path to official afl-tmin")
    parser.add_argument("--custom", help="Path to custom afl-tmin")
    parser.add_argument("binary", nargs="+",
                        help="Testing binary (@@ for filename)")
    args = parser.parse_args()
    assert os.path.isdir(args.input_dir), args.input_dir
    assert os.path.isdir(args.test_case_dir), args.test_case_dir
    assert os.path.isfile(args.official), args.official
    assert os.path.isfile(args.custom), args.custom
    if os.path.exists(args.output_dir):
        shutil.rmtree(args.output_dir)
    os.makedirs(args.output_dir)

    monitor = CrashMonitor(
        args.output_dir,
        run_afl_tmin(args.official, args.test_case_dir, args.binary),
        run_afl_tmin(args.custom, args.test_case_dir, args.binary, True)
    )
    print("===> Loading initial crashes...")
    for crash_file in os.listdir(args.input_dir):
        if crash_file.lower() == "readme.txt":
            continue
        real_path = os.path.join(args.input_dir, crash_file)
        if os.path.isfile(real_path):
            monitor.process(real_path)

    print("\n===> Observing crash directory...")
    observer = Observer()
    observer.schedule(monitor, args.input_dir, recursive=False)
    observer.start()
    try:
        while observer.is_alive():
            observer.join(1)
    except KeyboardInterrupt:
        print("Ending...")
    finally:
        observer.stop()
        observer.join()


# Call main
if __name__ == "__main__":
    main()
