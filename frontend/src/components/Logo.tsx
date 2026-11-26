export function Logo({ size = 28 }: { size?: number }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 32 32"
      fill="none"
      role="img"
      aria-label="moLink"
    >
      <defs>
        <linearGradient
          id="molink-mark"
          x1="4"
          y1="3"
          x2="28"
          y2="30"
          gradientUnits="userSpaceOnUse"
        >
          <stop stopColor="#0EA5A4" />
          <stop offset="1" stopColor="#3B82F6" />
        </linearGradient>
        <radialGradient
          id="molink-glow"
          cx="6"
          cy="5"
          r="24"
          gradientUnits="userSpaceOnUse"
        >
          <stop stopColor="white" stopOpacity="0.1" />
          <stop offset="1" stopColor="white" stopOpacity="0" />
        </radialGradient>
      </defs>
      <rect x="1" y="1" width="30" height="30" rx="10" fill="url(#molink-mark)" />
      <rect x="1" y="1" width="30" height="30" rx="10" fill="url(#molink-glow)" />
      <rect
        x="2"
        y="2"
        width="28"
        height="28"
        rx="9"
        stroke="white"
        strokeOpacity="0.18"
        strokeWidth="1"
      />
      <path
        d="M8.4 22V13.1a4.3 4.3 0 0 1 8.6 0V22M17 13.1a4.3 4.3 0 0 1 8.6 0V22"
        stroke="white"
        strokeWidth="2.8"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </svg>
  )
}
