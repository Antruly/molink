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
          <stop stopColor="#2dd4bf" />
          <stop offset="1" stopColor="#38bdf8" />
        </linearGradient>
      </defs>
      <rect x="1" y="1" width="30" height="30" rx="9" fill="url(#molink-mark)" />
      <path
        d="M8.4 23V13.1a4.3 4.3 0 0 1 8.6 0V23M17 13.1a4.3 4.3 0 0 1 8.6 0V23"
        stroke="white"
        strokeWidth="2.6"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </svg>
  )
}
