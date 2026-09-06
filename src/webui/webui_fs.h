//
// webui_fs.h
//
// Read-only filesystem endpoints for the web UI. All access is confined
// to the configured SD card volume and sandboxed against path traversal.
// FatFs calls are serialised with the emulator by FF_FS_REENTRANT; reads
// are chunked so a transfer never holds the volume lock for long.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _webui_fs_h
#define _webui_fs_h

class CSocket;

// GET /api/volumes  -> JSON list of browsable volumes with size/free.
void WebUiFsVolumes(CSocket *socket);

// GET /api/fs/list?vol=SD&path=/dir  -> JSON directory listing.
void WebUiFsList(CSocket *socket, const char *query);

// GET /api/fs/download?vol=SD&path=/dir/file  -> raw file bytes.
void WebUiFsDownload(CSocket *socket, const char *query);

#endif
