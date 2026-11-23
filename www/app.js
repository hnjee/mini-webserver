// 이 문구가 바뀌면 app.js가 올바른 MIME 타입으로 전달된 거예요.
const status = document.getElementById("js-status");
status.textContent = "JS가 정상으로 실행됐어요.";
status.className = "ok";
