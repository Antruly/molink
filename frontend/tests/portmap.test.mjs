import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import {
  EMPTY_PORTMAP_FORM,
  PORTMAP_PRESETS,
  applyPortmapPreset,
  buildPortmapInput,
  formatPortmapBytes,
  mappingStatus,
  portForwardEnabled,
  portForwardSaveValues,
  portmapErrorKey,
} from '../src/lib/portmap.ts'

const zh = JSON.parse(
  readFileSync(new URL('../src/locales/zh-CN.json', import.meta.url)),
)
const en = JSON.parse(
  readFileSync(new URL('../src/locales/en.json', import.meta.url)),
)

function translate(key) {
  return key.split('.').reduce((value, part) => value?.[part], zh)
}

function mapping(overrides = {}) {
  return {
    id: 1,
    listen_host: '127.0.0.1',
    listen_port: 13389,
    target_host: '127.0.0.1',
    target_port: 3389,
    enabled: true,
    listening: true,
    bytes_in: 0,
    bytes_out: 0,
    last_error: '',
    ...overrides,
  }
}

test('preset buttons fill the target port and leave the local port random', () => {
  assert.equal(PORTMAP_PRESETS.length, 6)
  assert.deepEqual(
    PORTMAP_PRESETS.map((preset) => [preset.id, preset.port]),
    [
      ['rdp', 3389],
      ['ssh', 22],
      ['mysql', 3306],
      ['http', 80],
      ['https', 443],
      ['smb', 445],
    ],
  )

  const filled = applyPortmapPreset(EMPTY_PORTMAP_FORM, PORTMAP_PRESETS[0])
  assert.equal(filled.listenPort, '')
  assert.equal(filled.targetHost, '127.0.0.1')
  assert.equal(filled.targetPort, '3389')
  // The preset must not mutate the shared empty form.
  assert.equal(EMPTY_PORTMAP_FORM.targetPort, '')

  const typed = applyPortmapPreset(
    { listenPort: '13389', targetHost: ' host.lan ', targetPort: '' },
    PORTMAP_PRESETS[2],
  )
  assert.equal(typed.listenPort, '')
  assert.equal(typed.targetHost, 'host.lan')
  assert.equal(typed.targetPort, '3306')
})

test('the add form turns into the API request the backend expects', () => {
  const result = buildPortmapInput({
    listenPort: '',
    targetHost: ' 10.0.0.5 ',
    targetPort: '3306',
  })
  assert.deepEqual(result, {
    ok: true,
    input: {
      listen_host: '127.0.0.1',
      listen_port: 0,
      target_host: '10.0.0.5',
      target_port: 3306,
    },
  })

  assert.deepEqual(
    buildPortmapInput({
      listenPort: '13306',
      targetHost: '127.0.0.1',
      targetPort: '3306',
    }),
    {
      ok: true,
      input: {
        listen_host: '127.0.0.1',
        listen_port: 13306,
        target_host: '127.0.0.1',
        target_port: 3306,
      },
    },
  )

  for (const listenPort of ['0', '65536', '-1', '12.5', 'abc']) {
    assert.deepEqual(
      buildPortmapInput({
        listenPort,
        targetHost: '127.0.0.1',
        targetPort: '3389',
      }),
      { ok: false, error: 'bad_port' },
      `listen port ${listenPort}`,
    )
  }
  assert.deepEqual(
    buildPortmapInput({
      listenPort: '',
      targetHost: '   ',
      targetPort: '3389',
    }),
    { ok: false, error: 'bad_target' },
  )
  for (const targetPort of ['', '0', '70000']) {
    assert.deepEqual(
      buildPortmapInput({
        listenPort: '',
        targetHost: '127.0.0.1',
        targetPort,
      }),
      { ok: false, error: 'bad_port' },
      `target port ${targetPort}`,
    )
  }
})

test('list rows choose the right status light and traffic text', () => {
  assert.equal(mappingStatus(mapping()), 'listening')
  assert.equal(
    mappingStatus(mapping({ last_error: 'port_forward_disabled' })),
    'error',
  )
  assert.equal(
    mappingStatus(mapping({ listening: false, enabled: false })),
    'disabled',
  )
  assert.equal(
    mappingStatus(mapping({ listening: false, enabled: true })),
    'stopped',
  )

  assert.equal(formatPortmapBytes(0), '0 B')
  assert.equal(formatPortmapBytes(1536), '1.5 KB')
  assert.equal(formatPortmapBytes(2 * 1024 * 1024), '2.0 MB')
  assert.equal(formatPortmapBytes(Number.NaN), '0 B')
})

test('the host switch reads /api/settings and preserves every other field', () => {
  // Older cores omit the field; the backend default is off.
  assert.equal(portForwardEnabled(null), false)
  assert.equal(portForwardEnabled({}), false)
  assert.equal(portForwardEnabled({ port_forward_enabled: false }), false)
  assert.equal(portForwardEnabled({ port_forward_enabled: true }), true)

  const security = {
    allow_remote_access: true,
    allow_web_client: false,
    discovery_enabled: true,
    allow_access_requests: false,
    device_name: 'studio',
    allow_sync_between_sessions: true,
    encrypt_remote: false,
    encrypt_lan: true,
    stun_server: 'stun.example:3478',
    ip_allowlist: '192.168.0.0/24',
    port_forward_enabled: false,
  }
  assert.deepEqual(portForwardSaveValues(security, true), {
    allowRemote: true,
    allowWeb: false,
    discovery: true,
    allowAccessRequests: false,
    syncBetweenSessions: true,
    encryptRemote: false,
    encryptLan: true,
    name: 'studio',
    stunServer: 'stun.example:3478',
    ipAllowlist: '192.168.0.0/24',
    portForwardEnabled: true,
  })

  // Turning it off again changes only the flag; every access/transport value
  // still comes from the last GET.
  const off = portForwardSaveValues(security, false)
  assert.equal(off.portForwardEnabled, false)
  assert.deepEqual(
    { ...off, portForwardEnabled: true },
    portForwardSaveValues(security, true),
  )
})

test('the port-map host toggle copy ships in both languages', () => {
  assert.equal(
    translate('portmap.hostToggle'),
    '允许本机为对方建立端口映射隧道',
  )
  assert.equal(
    en.portmap.hostToggle,
    'Allow this host to open port-mapping tunnels for the peer',
  )
  assert.ok(translate('portmap.hostSaveFailed'))
  assert.ok(en.portmap.hostSaveFailed)
})

test('error codes map to the Chinese messages shipped in the locale', () => {
  assert.equal(portmapErrorKey('bad_port'), 'portmap.errors.bad_port')
  assert.equal(
    translate(portmapErrorKey('bad_port')),
    '端口号无效，应为 1–65535。',
  )
  assert.equal(
    translate(portmapErrorKey('port_in_use')),
    '端口已被占用或无法监听。',
  )
  assert.equal(
    translate(portmapErrorKey('not_found')),
    '这条映射已不存在。',
  )
  assert.equal(
    translate(portmapErrorKey('bad_target')),
    '目标地址无效，请填写 host:port。',
  )
  assert.equal(
    translate(portmapErrorKey('port_forward_disabled')),
    '端口映射未启用：被控端需要允许端口映射。',
  )
  // Unknown codes (older core, network failure) still produce a message.
  assert.equal(portmapErrorKey('something_new'), 'portmap.errors.unknown')
  assert.equal(
    translate(portmapErrorKey('something_new')),
    '操作失败，无法连接本机后台。',
  )
})
