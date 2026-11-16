"use strict";
document.addEventListener("keydown", event => {
  if (event.key === "Escape") { event.preventDefault(); document.querySelector("form").requestSubmit(); }
});
