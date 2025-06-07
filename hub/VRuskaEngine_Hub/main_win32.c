#include <windows.h>
#pragma execution_character_set("utf-8")

// Обработка сообщений окна
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		// Используйте TextOutW для вывода Unicode-текста
		TextOutW(hdc, 50, 50, L"Добро пожаловать в VRuska Engine!", 35);

		EndPaint(hwnd, &ps);
		return 0;
	}
	}

	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	// Регистрация класса окна
	WNDCLASS wc = { 0 };
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"VRuskaHubWindowClass";

	RegisterClass(&wc);

	// Создание окна
	HWND hwnd = CreateWindow(
		L"VRuskaHubWindowClass",
		L"VRuska Engine - Hub",         // Заголовок окна (русский текст)
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
		NULL,
		NULL,
		hInstance,
		NULL
	);

	if (!hwnd) return 0;

	ShowWindow(hwnd, nCmdShow);

	// Добавим обработку WM_PAINT
	MSG msg = { 0 };

	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 0;
}
