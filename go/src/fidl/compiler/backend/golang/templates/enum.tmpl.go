// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const Enum = `
{{- define "EnumDefinition" -}}
type {{ .Name }} {{ .Type }}
const (
	{{- range .Members }}
	{{ $.Name }}{{ .Name }} {{ $.Name }} = {{ .Value }}
	{{- end }}
)
{{- end -}}
`
