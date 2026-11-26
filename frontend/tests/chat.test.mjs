import assert from 'node:assert/strict'
import test from 'node:test'
import {
  avatarColorFor,
  baseName,
  attachmentKind,
  chatDisplayName,
  chatPayloadKey,
  chatTextBytes,
  chatThumbName,
  chatUploadName,
  encodeChatPayload,
  extensionOf,
  fallbackAttachmentName,
  fitsChatText,
  formatDuration,
  formatSize,
  formatTime,
  formatTimeSeparator,
  historyPageCount,
  historyWindowStart,
  hostChatDirectory,
  iconFor,
  initialFor,
  isAllowedAttachmentPath,
  isNearBottom,
  isSameLocalDay,
  localDateString,
  makeChatAttachment,
  mimeForName,
  parseChatPayload,
  safeChatFileName,
  shouldInsertTimeSeparator,
  validateChatAttachment,
} from '../src/lib/chat.ts'

test('attachment kinds prefer the MIME type and fall back to the extension', () => {
  assert.equal(attachmentKind('shot.png', 'image/png'), 'image')
  assert.equal(attachmentKind('clip.MP4', 'video/mp4'), 'video')
  assert.equal(attachmentKind('photo.JPG', ''), 'image')
  assert.equal(attachmentKind('movie.MKV', 'application/octet-stream'), 'video')
  assert.equal(attachmentKind('report.docx', ''), 'file')
  assert.equal(attachmentKind('payload.bin', 'video/webm'), 'video')
  assert.equal(attachmentKind('payload.bin', 'image/webp'), 'image')
  assert.equal(attachmentKind('no-extension', ''), 'file')
  assert.equal(attachmentKind('archive.tar.gz', ''), 'file')
})

test('the icon table covers every documented family, case-insensitively', () => {
  const cases = [
    ['a.doc', 'doc'], ['a.DOCX', 'doc'], ['a.odt', 'doc'], ['a.rtf', 'doc'],
    ['a.xls', 'sheet'], ['a.XLSX', 'sheet'], ['a.csv', 'sheet'],
    ['a.ods', 'sheet'],
    ['a.ppt', 'slides'], ['a.pptx', 'slides'], ['a.odp', 'slides'],
    ['a.pdf', 'pdf'], ['a.PDF', 'pdf'],
    ['a.zip', 'archive'], ['a.rar', 'archive'], ['a.7z', 'archive'],
    ['a.tar', 'archive'], ['a.gz', 'archive'],
    ['a.png', 'image'], ['a.JPEG', 'image'], ['a.gif', 'image'],
    ['a.webp', 'image'], ['a.svg', 'image'],
    ['a.mp4', 'video'], ['a.MOV', 'video'], ['a.mkv', 'video'],
    ['a.avi', 'video'], ['a.webm', 'video'],
    ['a.mp3', 'audio'], ['a.WAV', 'audio'], ['a.flac', 'audio'],
    ['a.m4a', 'audio'],
    ['a.txt', 'text'], ['a.md', 'text'], ['a.log', 'text'],
    ['a.ts', 'code'], ['a.PY', 'code'], ['a.json', 'code'],
    ['a.rs', 'code'], ['a.html', 'code'],
    ['a.bin', 'file'], ['README', 'file'], ['.gitignore', 'file'],
  ]
  for (const [name, icon] of cases) {
    assert.equal(iconFor(name), icon, name)
  }
})

test('extensions and base names handle both separators', () => {
  assert.equal(extensionOf('a/b/Report.DOCX'), 'docx')
  assert.equal(extensionOf('C:\\tmp\\report.docx'), 'docx')
  assert.equal(extensionOf('README'), '')
  assert.equal(extensionOf('.gitignore'), '')
  assert.equal(extensionOf('trailing.'), '')
  assert.equal(baseName('C:\\tmp\\report.docx'), 'report.docx')
  assert.equal(baseName('a/b/c.txt'), 'c.txt')
  assert.equal(baseName('a/b/'), 'b')
  assert.equal(baseName(''), '')
})

test('formatSize uses binary units and one decimal', () => {
  assert.equal(formatSize(0), '0 B')
  assert.equal(formatSize(999), '999 B')
  assert.equal(formatSize(1024), '1 KB')
  assert.equal(formatSize(1536), '1.5 KB')
  assert.equal(formatSize(123456), '120.6 KB')
  assert.equal(formatSize(5 * 1024 * 1024), '5 MB')
  assert.equal(formatSize(2.5 * 1024 * 1024 * 1024), '2.5 GB')
  assert.equal(formatSize(-1), '—')
  assert.equal(formatSize(Number.NaN), '—')
})

test('formatTime is a locale-independent 24-hour clock', () => {
  assert.equal(formatTime(0), '')
  assert.equal(formatTime(-1), '')
  const stamp = new Date(2024, 0, 2, 9, 5).getTime()
  assert.equal(formatTime(stamp), '09:05')
  const evening = new Date(2024, 0, 2, 23, 59).getTime()
  assert.equal(formatTime(evening), '23:59')
})

test('time separators appear after a five-minute gap and at the start', () => {
  assert.equal(shouldInsertTimeSeparator(0, 1000), true)
  assert.equal(shouldInsertTimeSeparator(1000, 1000 + 5 * 60_000), false)
  assert.equal(shouldInsertTimeSeparator(1000, 1000 + 5 * 60_000 + 1), true)
  assert.equal(shouldInsertTimeSeparator(1000, 500), false)
  assert.equal(shouldInsertTimeSeparator(1000, 0), false)
  assert.equal(shouldInsertTimeSeparator(1000, Number.NaN), false)
})

test('the separator label includes the date when it is not today', () => {
  const stamp = new Date(2024, 0, 2, 10, 30).getTime()
  assert.equal(
    formatTimeSeparator(stamp, new Date(2024, 0, 2, 12, 0).getTime()),
    '10:30',
  )
  assert.equal(
    formatTimeSeparator(stamp, new Date(2024, 0, 3, 12, 0).getTime()),
    '2024-01-02 10:30',
  )
  assert.equal(formatTimeSeparator(0), '')
  assert.equal(
    isSameLocalDay(stamp, new Date(2024, 0, 2, 23, 59).getTime()),
    true,
  )
  assert.equal(
    isSameLocalDay(stamp, new Date(2024, 0, 3, 0, 0).getTime()),
    false,
  )
})

test('video durations are m:ss and h:mm:ss', () => {
  assert.equal(formatDuration(0), '')
  assert.equal(formatDuration(7), '0:07')
  assert.equal(formatDuration(65), '1:05')
  assert.equal(formatDuration(3723), '1:02:03')
  assert.equal(formatDuration(Number.NaN), '')
})

test('MIME fallbacks are derived from the extension', () => {
  assert.equal(mimeForName('a.PNG'), 'image/png')
  assert.equal(mimeForName('a.mp4'), 'video/mp4')
  assert.equal(mimeForName('a.docx'), 'application/vnd.openxmlformats-officedocument.wordprocessingml.document')
  assert.equal(mimeForName('a.unknown'), 'application/octet-stream')
})

test('an attachment descriptor without a name or a size is rejected', () => {
  assert.equal(validateChatAttachment(null), null)
  assert.equal(validateChatAttachment('x'), null)
  assert.equal(validateChatAttachment({ size: 1 }), null)
  assert.equal(validateChatAttachment({ name: 'a.txt' }), null)
  assert.equal(validateChatAttachment({ name: 'a.txt', size: -1 }), null)
  assert.equal(validateChatAttachment({ name: '  ', size: 1 }), null)
  assert.equal(
    validateChatAttachment({ name: 'a.txt', size: Number.NaN }),
    null,
  )
  const valid = validateChatAttachment({
    id: 'abc',
    name: '报表.docx',
    size: 123456,
    mime: 'application/msword',
  })
  assert.ok(valid)
  assert.equal(valid.name, '报表.docx')
  assert.equal(valid.size, 123456)
  assert.equal(valid.kind, 'file')
  assert.equal(valid.thumb, '')
  // A zero-byte file is still a file.
  const empty = validateChatAttachment({ name: 'empty.txt', size: 0 })
  assert.ok(empty)
  assert.equal(empty.size, 0)
})

test('a descriptor repairs a missing kind and derives it when needed', () => {
  const repaired = validateChatAttachment({
    name: 'shot.png',
    size: 10,
    kind: 'nonsense',
    w: -2,
    h: 3.9,
    dur: 'x',
  })
  assert.ok(repaired)
  assert.equal(repaired.kind, 'image')
  assert.equal(repaired.w, 0)
  assert.equal(repaired.h, 3)
  assert.equal(repaired.dur, 0)
  assert.equal(makeChatAttachment({ id: 'i', name: 'v.mp4', size: 3, mime: '' }).kind, 'video')
})

test('parseChatPayload ignores plain text and rejects malformed envelopes', () => {
  assert.equal(parseChatPayload('hello'), null)
  assert.equal(parseChatPayload('{not json'), null)
  assert.equal(parseChatPayload('{"type":"other"}'), null)
  assert.equal(parseChatPayload('{"type":"chat"}')?.attachments.length, 0)
  assert.equal(parseChatPayload('[]'), null)

  const payload = parseChatPayload(JSON.stringify({
    type: 'chat',
    text: '看这个',
    attachments: [
      { id: 'a', name: '报表.docx', size: 12, mime: '', kind: 'file' },
      { id: 'bad' },
    ],
  }))
  assert.ok(payload)
  assert.equal(payload.text, '看这个')
  assert.equal(payload.attachments.length, 1)
  assert.equal(payload.attachments[0].name, '报表.docx')
})

test('the message envelope round-trips and stays inside the chat limit', () => {
  const attachment = makeChatAttachment({
    id: 'abc123',
    name: 'photo.webp',
    size: 4096,
    mime: 'image/webp',
    thumb: 'C:\\Users\\me\\moLink\\x\\chat\\thumb-abc123.webp',
    src: 'C:\\Users\\me\\moLink\\x\\chat\\photo.webp',
    w: 320,
    h: 240,
  })
  const encoded = encodeChatPayload({ type: 'chat', text: 'hi', attachments: [attachment] })
  const parsed = parseChatPayload(encoded)
  assert.ok(parsed)
  assert.deepEqual(parsed.attachments[0], attachment)
  assert.equal(chatTextBytes(encoded) > 0, true)
  assert.equal(fitsChatText(encoded), true)
  assert.equal(fitsChatText('x'.repeat(4096)), true)
  assert.equal(fitsChatText('x'.repeat(4097)), false)
  assert.equal(chatPayloadKey(parsed), 'hi\u0000abc123')
})

test('safeChatFileName strips traversal, separators and reserved names', () => {
  assert.equal(safeChatFileName('../../etc/passwd'), 'passwd')
  assert.equal(safeChatFileName('C:\\tmp\\a.txt'), 'a.txt')
  assert.equal(safeChatFileName('a<b>c:d.txt'), 'a_b_c_d.txt')
  assert.equal(safeChatFileName('  trailing.  '), 'trailing')
  assert.equal(safeChatFileName(''), 'file')
  assert.equal(safeChatFileName('CON'), '_CON')
  assert.equal(safeChatFileName('con.txt'), '_con.txt')
  assert.equal(safeChatFileName('console.txt'), 'console.txt')
  assert.equal(safeChatFileName('报表.docx'), '报表.docx')
  const long = safeChatFileName(`${'x'.repeat(400)}.txt`)
  assert.ok(long.length <= 180)
  assert.ok(long.endsWith('.txt'))
})

test('a pasted image without a name gets a type-derived one', () => {
  assert.equal(fallbackAttachmentName('image/png', 'abc123'), 'image-abc123.png')
  assert.equal(fallbackAttachmentName('image/jpeg', ''), 'image.jpg')
  assert.equal(fallbackAttachmentName('video/mp4', 'v1'), 'video-v1.mp4')
  assert.equal(fallbackAttachmentName('application/pdf', ''), 'file.bin')
  assert.equal(chatDisplayName('  report.pdf ', 'application/pdf'), 'report.pdf')
  assert.equal(
    chatDisplayName('', 'image/webp', 'a/b'),
    'image-ab.webp',
  )
})

test('upload paths stay under the host chat folder', () => {
  assert.equal(chatUploadName('报表.docx'), 'chat/报表.docx')
  assert.equal(chatUploadName('../x.png'), 'chat/x.png')
  assert.equal(chatThumbName('abc-123'), 'chat/thumb-abc-123.webp')
  assert.equal(chatThumbName('../../'), 'chat/thumb-image.webp')
})

test('host chat directories use the folder separator and sanitize the id', () => {
  assert.equal(
    hostChatDirectory('C:\\Users\\me\\moLink', 'AB3-DEF', '2026-11-26'),
    'C:\\Users\\me\\moLink\\AB3-DEF\\2026-11-26\\chat',
  )
  assert.equal(
    hostChatDirectory('/home/me/moLink/', 'a/b', '2026-11-26'),
    '/home/me/moLink/a_b/2026-11-26/chat',
  )
  assert.equal(hostChatDirectory('', 'x', '2026-11-26'), '')
  assert.equal(hostChatDirectory('/root', '..', 'day'), '/root/unknown/day/chat')
})

test('received attachment paths must stay in a chat folder under the root', () => {
  const root = '/home/me/moLink'
  assert.equal(
    isAllowedAttachmentPath(
      root,
      '/home/me/moLink/ab3/2026-11-26/chat/report.docx',
    ),
    true,
  )
  assert.equal(
    isAllowedAttachmentPath(
      root,
      '/home/me/moLink/ab3/2026-11-26/chat/thumbs/thumb.webp',
    ),
    true,
  )
  assert.equal(
    isAllowedAttachmentPath(
      'C:\\Users\\me\\moLink',
      'c:\\users\\me\\molink\\AB3\\2026-11-26\\chat\\a.png',
    ),
    true,
  )
  // Outside the configured root, traversal, and a `chat` file name instead of
  // a folder are all refused.
  assert.equal(
    isAllowedAttachmentPath(root, '/etc/passwd'),
    false,
  )
  assert.equal(
    isAllowedAttachmentPath(root, '/home/me/moLink/ab3/chat/../secret'),
    false,
  )
  assert.equal(
    isAllowedAttachmentPath(root, '/home/me/moLink/ab3/2026-11-26/chat'),
    false,
  )
  assert.equal(isAllowedAttachmentPath('', '/a/chat/b'), false)
  assert.equal(isAllowedAttachmentPath(root, ''), false)
})

test('scroll and history helpers agree on the newest page', () => {
  assert.equal(isNearBottom(900, 1000, 200, 20), true)
  assert.equal(isNearBottom(700, 1000, 200, 20), false)
  assert.equal(isNearBottom(0, 100, 100), true)
  assert.equal(historyWindowStart(100, 40), 60)
  assert.equal(historyWindowStart(10, 40), 0)
  assert.equal(historyPageCount(100, 40), 40)
  assert.equal(historyPageCount(10, 40), 10)
  assert.equal(historyPageCount(0, 40), 0)
  assert.equal(historyPageCount(100, -5), 0)
})

test('avatar initials and colours are stable', () => {
  assert.equal(initialFor('Alice'), 'A')
  assert.equal(initialFor(' 张三 '), '张')
  assert.equal(initialFor(''), '?')
  assert.equal(avatarColorFor('Alice'), avatarColorFor('Alice'))
  assert.match(avatarColorFor('Alice'), /^#[0-9a-f]{6}$/)
  assert.equal(localDateString(new Date(2026, 10, 26)), '2026-11-26')
})
