# Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of Sciences
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Resolve modern upstream gem5 SE workload and suite resources.

This is intentionally a narrow compatibility layer for this fork's older
resource implementation. It consumes the current upstream catalog schema but
only implements what ``configs/example/se.py`` needs: ``binary``, ``file``,
``workload``, and ``suite`` resources using ``set_se_binary_workload``.
"""

import gzip
import hashlib
import json
import os
import shutil
import time
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple
from urllib import parse, request

from ..utils.filelock import FileLock


DEFAULT_RESOURCE_API = "https://api.gem5.org/api/resources"


class ResourceCatalogError(Exception):
    """Raised when resource metadata cannot be resolved or used by SE."""


class ResourceReference:
    def __init__(
        self,
        id: str,
        resource_version: str,
        input_groups: Tuple[str, ...] = (),
    ):
        self.id = id
        self.resource_version = resource_version
        self.input_groups = input_groups


class SESuite:
    def __init__(
        self,
        id: str,
        resource_version: str,
        workloads: Tuple[ResourceReference, ...],
    ):
        self.id = id
        self.resource_version = resource_version
        self.workloads = workloads

    def __iter__(self):
        yield from self.workloads

    def __len__(self):
        return len(self.workloads)

    def get_input_groups(self):
        return {
            input_group
            for workload in self.workloads
            for input_group in workload.input_groups
        }

    def with_input_group(self, input_group: str) -> "SESuite":
        if input_group not in self.get_input_groups():
            raise ResourceCatalogError(
                "Input group '{}' is not in suite '{}'.".format(
                    input_group, self.id
                )
            )
        return SESuite(
            id=self.id,
            resource_version=self.resource_version,
            workloads=tuple(
                workload for workload in self.workloads
                if input_group in workload.input_groups
            ),
        )

    def get_workload(self, workload_id: str) -> ResourceReference:
        for workload in self.workloads:
            if workload.id == workload_id:
                return workload
        available = ", ".join(workload.id for workload in self.workloads)
        raise ResourceCatalogError(
            "Workload '{}' is not in suite '{}'. Available workloads: {}"
            .format(workload_id, self.id, available)
        )


class SEWorkload:
    def __init__(
        self,
        id: str,
        resource_version: str,
        executable: str,
        architecture: Optional[str] = None,
        arguments: Tuple[str, ...] = (),
        stdin_file: Optional[str] = None,
        stdout_file: Optional[str] = None,
        stderr_file: Optional[str] = None,
        env_list: Optional[Tuple[str, ...]] = None,
    ):
        self.id = id
        self.resource_version = resource_version
        self.executable = executable
        self.architecture = architecture
        self.arguments = arguments
        self.stdin_file = stdin_file
        self.stdout_file = stdout_file
        self.stderr_file = stderr_file
        self.env_list = env_list


def _version_key(version: str) -> Tuple:
    parts = []
    for part in version.replace("-", ".").split("."):
        parts.append((0, int(part)) if part.isdigit() else (1, part))
    return tuple(parts)


class ResourceCatalog:
    """A small client for the upstream resource API or a flat JSON catalog."""

    def __init__(self, source: Optional[str] = None):
        self.source = source or os.getenv(
            "GEM5_RESOURCE_API", DEFAULT_RESOURCE_API
        )
        self._local_resources = None
        self._local_base = None

        if Path(self.source).is_file():
            source_path = Path(self.source).resolve()
            with source_path.open() as catalog_file:
                contents = json.load(catalog_file)
            if isinstance(contents, dict):
                contents = contents.get("resources")
            if not isinstance(contents, list):
                raise ResourceCatalogError(
                    "A local modern resource catalog must be a JSON list or "
                    "an object containing a 'resources' list."
                )
            self._local_resources = contents
            self._local_base = source_path.parent
        elif source is not None and not parse.urlparse(self.source).scheme:
            raise ResourceCatalogError(
                "Local modern resource catalog '{}' does not exist."
                .format(self.source)
            )

    def get_resources(
        self, references: Iterable[Tuple[str, Optional[str]]]
    ) -> List[Dict]:
        references = list(references)
        if not references:
            return []

        if self._local_resources is not None:
            return [
                self._get_local_resource(resource_id, resource_version)
                for resource_id, resource_version in references
            ]

        query = []
        for resource_id, resource_version in references:
            query.extend(
                (
                    ("id", resource_id),
                    ("resource_version", resource_version or "None"),
                )
            )
        url = "{}/find-resources-in-batch?{}".format(
            self.source.rstrip("/"), parse.urlencode(query)
        )
        resources = self._read_json_url(url)

        if not isinstance(resources, list):
            raise ResourceCatalogError(
                "The gem5 resource API returned an unexpected response."
            )

        resolved = []
        for resource_id, resource_version in references:
            candidates = [
                resource for resource in resources
                if resource.get("id") == resource_id
                and (
                    resource_version is None
                    or resource.get("resource_version") == resource_version
                )
            ]
            if not candidates:
                raise ResourceCatalogError(
                    "Resource '{}'{} was not found in the gem5 catalog."
                    .format(
                        resource_id,
                        " version '{}'".format(resource_version)
                        if resource_version else "",
                    )
                )
            resolved.append(
                max(
                    candidates,
                    key=lambda resource: _version_key(
                        resource["resource_version"]
                    ),
                )
            )
        return resolved

    def get_resource(
        self, resource_id: str, resource_version: Optional[str] = None
    ) -> Dict:
        return self.get_resources([(resource_id, resource_version)])[0]

    def get_suite(
        self, resource_id: str, resource_version: Optional[str] = None
    ) -> SESuite:
        metadata = self.get_resource(resource_id, resource_version)
        if metadata.get("category") != "suite":
            raise ResourceCatalogError(
                "Resource '{}' is '{}', not a suite.".format(
                    resource_id, metadata.get("category", "unknown")
                )
            )
        workloads = tuple(
            ResourceReference(
                id=workload["id"],
                resource_version=workload["resource_version"],
                input_groups=tuple(workload.get("input_group", [])),
            )
            for workload in metadata.get("workloads", [])
        )
        return SESuite(
            id=metadata["id"],
            resource_version=metadata["resource_version"],
            workloads=workloads,
        )

    def obtain_se_workload(
        self,
        resource_id: str,
        resource_version: Optional[str] = None,
        resource_directory: Optional[str] = None,
    ) -> SEWorkload:
        metadata = self.get_resource(resource_id, resource_version)
        if metadata.get("category") == "binary":
            executable = self._obtain_file(metadata, resource_directory)
            return SEWorkload(
                id=metadata["id"],
                resource_version=metadata["resource_version"],
                executable=executable,
                architecture=metadata.get("architecture"),
            )
        if metadata.get("category") != "workload":
            raise ResourceCatalogError(
                "Resource '{}' is '{}', not a workload.".format(
                    resource_id, metadata.get("category", "unknown")
                )
            )
        if metadata.get("function") != "set_se_binary_workload":
            raise ResourceCatalogError(
                "Workload '{}' uses unsupported function '{}'. Only "
                "set_se_binary_workload is supported by this SE entry point."
                .format(resource_id, metadata.get("function"))
            )

        resource_refs = metadata.get("resources", {})
        if "binary" not in resource_refs:
            raise ResourceCatalogError(
                "Workload '{}' does not define a binary resource."
                .format(resource_id)
            )
        supported_resource_params = {"binary", "stdin_file"}
        unsupported = set(resource_refs) - supported_resource_params
        if unsupported:
            raise ResourceCatalogError(
                "Workload '{}' uses unsupported resource parameters: {}"
                .format(resource_id, ", ".join(sorted(unsupported)))
            )

        ref_items = list(resource_refs.items())
        resolved_metadata = self.get_resources(
            (
                value["id"], value.get("resource_version")
            ) for _, value in ref_items
        )
        paths = {}
        parameter_metadata = {}
        for (name, _), resource in zip(ref_items, resolved_metadata):
            if resource.get("category") not in ("binary", "file"):
                raise ResourceCatalogError(
                    "Workload parameter '{}' refers to unsupported category "
                    "'{}'.".format(name, resource.get("category", "unknown"))
                )
            paths[name] = self._obtain_file(resource, resource_directory)
            parameter_metadata[name] = resource

        params = metadata.get("additional_params", {})
        supported_params = {
            "arguments", "env_list", "stdout_file", "stderr_file"
        }
        unsupported = set(params) - supported_params
        if unsupported:
            raise ResourceCatalogError(
                "Workload '{}' uses unsupported parameters: {}".format(
                    resource_id, ", ".join(sorted(unsupported))
                )
            )

        arguments = tuple(str(argument) for argument in params.get(
            "arguments", []
        ))
        env_list = params.get("env_list")
        if env_list is not None:
            env_list = tuple(str(item) for item in env_list)

        return SEWorkload(
            id=metadata["id"],
            resource_version=metadata["resource_version"],
            executable=paths["binary"],
            architecture=parameter_metadata["binary"].get("architecture"),
            arguments=arguments,
            stdin_file=paths.get("stdin_file"),
            stdout_file=params.get("stdout_file"),
            stderr_file=params.get("stderr_file"),
            env_list=env_list,
        )

    def _get_local_resource(
        self, resource_id: str, resource_version: Optional[str]
    ) -> Dict:
        candidates = [
            resource for resource in self._local_resources
            if resource.get("id") == resource_id
            and (
                resource_version is None
                or resource.get("resource_version") == resource_version
            )
        ]
        if not candidates:
            raise ResourceCatalogError(
                "Resource '{}'{} was not found in '{}'.".format(
                    resource_id,
                    " version '{}'".format(resource_version)
                    if resource_version else "",
                    self.source,
                )
            )
        return max(
            candidates,
            key=lambda resource: _version_key(resource["resource_version"]),
        )

    def _obtain_file(
        self, metadata: Dict, resource_directory: Optional[str]
    ) -> str:
        if not metadata.get("url"):
            raise ResourceCatalogError(
                "Resource '{}' has no download URL.".format(metadata["id"])
            )
        resource_directory = resource_directory or os.getenv(
            "GEM5_RESOURCE_DIR",
            str(Path.home() / ".cache" / "gem5"),
        )
        resource_dir = Path(resource_directory)
        resource_dir.mkdir(parents=True, exist_ok=True)
        destination = resource_dir / "{}-{}".format(
            metadata["id"], metadata["resource_version"]
        )

        with FileLock("{}.lock".format(destination), timeout=900):
            if destination.is_file() and self._hash_matches(
                destination, metadata
            ):
                return str(destination)
            if destination.exists():
                if not destination.is_file():
                    raise ResourceCatalogError(
                        "Resource destination '{}' is not a file."
                        .format(destination)
                    )
                destination.unlink()

            source = metadata["url"]
            if self._local_base is not None and not parse.urlparse(
                source
            ).scheme:
                source = str((self._local_base / source).resolve())

            download_path = Path("{}.download".format(destination))
            try:
                print(
                    "Resource '{}' was not found locally. Downloading to "
                    "'{}'...".format(metadata["id"], destination)
                )
                if (
                    not parse.urlparse(source).scheme
                    and Path(source).is_file()
                ):
                    shutil.copyfile(source, download_path)
                else:
                    self._download_url(source, download_path)
                if metadata.get("is_zipped", False):
                    with gzip.open(download_path, "rb") as compressed:
                        with destination.open("wb") as output:
                            shutil.copyfileobj(compressed, output)
                    download_path.unlink()
                else:
                    os.replace(download_path, destination)
                print(
                    "Finished downloading resource '{}'.".format(
                        metadata["id"]
                    )
                )
            finally:
                if download_path.exists():
                    download_path.unlink()

            if not self._hash_matches(destination, metadata):
                destination.unlink()
                raise ResourceCatalogError(
                    "Downloaded resource '{}' failed its MD5 check."
                    .format(metadata["id"])
                )
        return str(destination)

    def _read_json_url(self, url: str, max_attempts: int = 3):
        error = None
        for attempt in range(max_attempts):
            try:
                with request.urlopen(url) as response:
                    return json.loads(response.read().decode("utf-8"))
            except Exception as caught:
                error = caught
                if attempt + 1 < max_attempts:
                    time.sleep(2 ** (attempt + 1))
        raise ResourceCatalogError(
            "Unable to query the gem5 resource catalog at '{}': {}".format(
                self.source, error
            )
        ) from error

    @staticmethod
    def _download_url(url: str, destination: Path, max_attempts: int = 3):
        error = None
        for attempt in range(max_attempts):
            try:
                request.urlretrieve(url, destination)
                return
            except Exception as caught:
                error = caught
                if destination.exists():
                    destination.unlink()
                if attempt + 1 < max_attempts:
                    time.sleep(2 ** (attempt + 1))
        raise ResourceCatalogError(
            "Unable to download resource from '{}': {}".format(url, error)
        ) from error

    @staticmethod
    def _hash_matches(path: Path, metadata: Dict) -> bool:
        expected = metadata.get("md5sum")
        if expected is None:
            return True
        digest = hashlib.md5()
        with path.open("rb") as resource_file:
            for chunk in iter(lambda: resource_file.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest() == expected
