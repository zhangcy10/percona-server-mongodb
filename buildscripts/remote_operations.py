#!/usr/bin/env python

"""Remote access utilities, via ssh & scp."""

from __future__ import print_function

import optparse
import os
import posixpath
import re
import shlex
import sys
import time

# The subprocess32 module is untested on Windows and thus isn't recommended for use, even when it's
# installed. See https://github.com/google/python-subprocess32/blob/3.2.7/README.md#usage.
if os.name == "posix" and sys.version_info[0] == 2:
    try:
        import subprocess32 as subprocess
    except ImportError:
        import warnings
        warnings.warn(("Falling back to using the subprocess module because subprocess32 isn't"
                       " available. When using the subprocess module, a child process may trigger"
                       " an invalid free(). See SERVER-22219 for more details."),
                      RuntimeWarning)
        import subprocess
else:
    import subprocess

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

_is_windows = sys.platform == "win32" or sys.platform == "cygwin"

_OPERATIONS = ["shell", "copy_to", "copy_from"]


def posix_path(path):
    # If path is already quoted, we need to remove the quotes before calling
    path_quote = "\'" if path.startswith("\'") else ""
    path_quote = "\"" if path.startswith("\"") else path_quote
    if path_quote:
        path = path[1:-1]
    drive, new_path = os.path.splitdrive(path)
    if drive:
        new_path = posixpath.join(
            "/cygdrive",
            drive.split(":")[0],
            *re.split("/|\\\\", new_path))
    return "{quote}{path}{quote}".format(quote=path_quote, path=new_path)


class RemoteOperations(object):
    """Class to support remote operations."""

    def __init__(self,
                 user_host,
                 ssh_options="",
                 retries=0,
                 retry_sleep=0,
                 debug=False,
                 use_shell=False):

        self.user_host = user_host
        self.ssh_options = ssh_options if ssh_options else ""
        self.retries = retries
        self.retry_sleep = retry_sleep
        self.debug = debug
        self.use_shell = use_shell
        # Check if we can remotely access the host.
        self._access_code, self._access_buff = self._remote_access()

    def _call(self, cmd):
        if self.debug:
            print(cmd)
        # If use_shell is False we need to split the command up into a list.
        if not self.use_shell:
            cmd = shlex.split(cmd)
        # Use a common pipe for stdout & stderr for logging.
        process = subprocess.Popen(cmd,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT,
                                   shell=self.use_shell)
        buff_stdout, buff_stderr = process.communicate()
        return process.poll(), buff_stdout

    def _remote_access(self):
        """ This will check if a remote session is possible. """
        cmd = "ssh {} {} date".format(self.ssh_options, self.user_host)
        attempt_num = 0
        buff = ""
        while True:
            ret, buff = self._call(cmd)
            if not ret:
                return ret, buff
            attempt_num += 1
            if attempt_num > self.retries:
                break
            if self.debug:
                print("Failed remote attempt {}, retrying in {} seconds".format(
                    attempt_num, self.retry_sleep))
            time.sleep(self.retry_sleep)
        return ret, buff

    def _perform_operation(self, cmd):
        return self._call(cmd)

    def access_established(self):
        """ Returns True if initial access was establsished. """
        return not self._access_code

    def access_info(self):
        """ Returns return code and output buffer from initial access attempt(s). """
        return self._access_code, self._access_buff

    def operation(self, operation_type, operation_param, operation_dir=None):
        """ Main entry for remote operations. Returns (code, output).

            'operation_type' supports remote shell and copy operations.
            'operation_param' can either be a list or string of commands or files.
            'operation_dir' is '.' if unspecified for 'copy_*'.
        """

        if not self.access_established():
            return self.access_info()

        # File names with a space must be quoted, since we permit the
        # the file names to be either a string or a list.
        if operation_type.startswith("copy") and isinstance(operation_param, str):
            operation_param = shlex.split(operation_param, posix=not _is_windows)

        cmds = []
        if operation_type == "shell":
            if operation_dir is not None:
                operation_param = "cd {}; {}".format(operation_dir, operation_param)
            dollar = ""
            if self.use_shell:
                # To ensure any single quotes in operation_param are handled correctly when
                # invoking the operation_param, escape with \ and add $ in the front.
                # See https://stackoverflow.com/questions/8254120/
                #   how-to-escape-a-single-quote-in-single-quote-string-in-bash
                if "'" in operation_param:
                    operation_param = "{}".format(operation_param.replace("'", "\\'"))
                    dollar = "$"
            cmd = "ssh {} {} {}'{}'".format(
                self.ssh_options,
                self.user_host,
                dollar,
                operation_param)
            cmds.append(cmd)

        elif operation_type == "copy_to":
            cmd = "scp -r {} ".format(self.ssh_options)
            # To support spaces in the filename or directory, we quote them one at a time.
            for file in operation_param:
                # Quote file on Posix.
                quote = "\"" if not _is_windows else ""
                cmd += "{quote}{file}{quote} ".format(quote=quote, file=posix_path(file))
            operation_dir = operation_dir if operation_dir else ""
            cmd += " {}:{}".format(self.user_host, posix_path(operation_dir))
            cmds.append(cmd)

        elif operation_type == "copy_from":
            operation_dir = operation_dir if operation_dir else "."
            if not os.path.isdir(operation_dir):
                raise ValueError(
                    "Local directory '{}' does not exist.".format(operation_dir))

            # We support multiple files being copied from the remote host
            # by invoking scp for each file specified.
            # Note - this is a method which scp does not support directly.
            for file in operation_param:
                file = posix_path(file)
                cmd = "scp -r {} {}:".format(self.ssh_options, self.user_host)
                # Quote (on Posix), and escape the file if there are spaces.
                # Note - we do not support other non-ASCII characters in a file name.
                quote = "\"" if not _is_windows else ""
                if " " in file:
                    file = re.escape("{quote}{file}{quote}".format(quote=quote, file=file))
                cmd += "{} {}".format(file, posix_path(operation_dir))
                cmds.append(cmd)

        else:
            raise ValueError(
                "Invalid operation '{}' specified, choose from {}.".format(
                    operation_type, _OPERATIONS))

        final_ret = 0
        for cmd in cmds:
            ret, buff = self._perform_operation(cmd)
            buff += buff
            final_ret = final_ret or ret

        return final_ret, buff

    def shell(self, operation_param, operation_dir=None):
        """ Helper for remote shell operations. """
        return self.operation(
            operation_type="shell",
            operation_param=operation_param,
            operation_dir=operation_dir)

    def copy_to(self, operation_param, operation_dir=None):
        """ Helper for remote copy_to operations. """
        return self.operation(
            operation_type="copy_to",
            operation_param=operation_param,
            operation_dir=operation_dir)

    def copy_from(self, operation_param, operation_dir=None):
        """ Helper for remote copy_from operations. """
        return self.operation(
            operation_type="copy_from",
            operation_param=operation_param,
            operation_dir=operation_dir)


def main():

    parser = optparse.OptionParser(description=__doc__)
    control_options = optparse.OptionGroup(parser, "Control options")
    shell_options = optparse.OptionGroup(parser, "Shell options")
    copy_options = optparse.OptionGroup(parser, "Copy options")

    parser.add_option("--userHost",
                      dest="user_host",
                      default=None,
                      help="User and remote host to execute commands on [REQUIRED]."
                           " Examples, 'user@1.2.3.4' or 'user@myhost.com'.")

    parser.add_option("--operation",
                      dest="operation",
                      default="shell",
                      choices=_OPERATIONS,
                      help="Remote operation to perform, choose one of '{}',"
                           " defaults to '%default'.".format(", ".join(_OPERATIONS)))

    control_options.add_option("--sshOptions",
                               dest="ssh_options",
                               default=None,
                               action="append",
                               help="SSH connection options."
                                    " More than one option can be specified either"
                                    " in one quoted string or by specifying"
                                    " this option more than once. Example options:"
                                    " '-i $HOME/.ssh/access.pem -o ConnectTimeout=10"
                                    " -o ConnectionAttempts=10'")

    control_options.add_option("--retries",
                               dest="retries",
                               type=int,
                               default=0,
                               help="Number of retries to attempt for operation,"
                                    " defaults to '%default'.")

    control_options.add_option("--retrySleep",
                               dest="retry_sleep",
                               type=int,
                               default=10,
                               help="Number of seconds to wait between retries,"
                                    " defaults to '%default'.")

    control_options.add_option("--debug",
                               dest="debug",
                               action="store_true",
                               default=False,
                               help="Provides debug output.")

    control_options.add_option("--verbose",
                               dest="verbose",
                               action="store_true",
                               default=False,
                               help="Print exit status and output at end.")

    shell_options.add_option("--commands",
                             dest="remote_commands",
                             default=None,
                             action="append",
                             help="Commands to excute on the remote host. The"
                                  " commands must be separated by a ';' and can either"
                                  " be specifed in a quoted string or by specifying"
                                  " this option more than once. A ';' will be added"
                                  " between commands when this option is specifed"
                                  " more than once.")

    shell_options.add_option("--commandDir",
                             dest="command_dir",
                             default=None,
                             help="Working directory on remote to execute commands"
                                  " form. Defaults to remote login directory.")

    copy_options.add_option("--file",
                            dest="files",
                            default=None,
                            action="append",
                            help="The file to copy to/from remote host. To"
                                 " support spaces in the file, each file must be"
                                 " specified using this option more than once.")

    copy_options.add_option("--remoteDir",
                            dest="remote_dir",
                            default=None,
                            help="Remote directory to copy to, only applies when"
                                 " operation is 'copy_to'. Defaults to the login"
                                 " directory on the remote host.")

    copy_options.add_option("--localDir",
                            dest="local_dir",
                            default=".",
                            help="Local directory to copy to, only applies when"
                                 " operation is 'copy_from'. Defaults to the"
                                 " current directory, '%default'.")

    parser.add_option_group(control_options)
    parser.add_option_group(shell_options)
    parser.add_option_group(copy_options)

    (options, args) = parser.parse_args()

    if not getattr(options, "user_host", None):
        parser.print_help()
        parser.error("Missing required option")

    if options.operation == "shell":
        if not getattr(options, "remote_commands", None):
            parser.print_help()
            parser.error("Missing required '{}' option '{}'".format(
                options.operation, "--commands"))
        operation_param = ";".join(options.remote_commands)
        operation_dir = options.command_dir
    else:
        if not getattr(options, "files", None):
            parser.print_help()
            parser.error("Missing required '{}' option '{}'".format(
                options.operation, "--file"))
        operation_param = options.files
        if options.operation == "copy_to":
            operation_dir = options.remote_dir
        else:
            operation_dir = options.local_dir

    ssh_options = None if not options.ssh_options else " ".join(options.ssh_options)
    remote_op = RemoteOperations(
        user_host=options.user_host,
        ssh_options=ssh_options,
        retries=options.retries,
        retry_sleep=options.retry_sleep,
        debug=options.debug)
    ret_code, buffer = remote_op.operation(options.operation, operation_param, operation_dir)
    if options.verbose:
        print("Return code: {} for command {}".format(ret_code, sys.argv))
        print(buffer)

    sys.exit(ret_code)


if __name__ == "__main__":
    main()
