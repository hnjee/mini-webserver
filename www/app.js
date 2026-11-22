// 정적 파일 서빙과 자바스크립트 MIME 매핑 확인용
document.addEventListener("DOMContentLoaded", function () {
  var status = document.getElementById("status");
  if (status) {
    status.textContent = "app.js를 내려받아 실행했다.";
  }
});
