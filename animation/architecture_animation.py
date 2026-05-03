from manim import *


class ZeroMQNotebookArchitecture(Scene):
    def make_node(self, title, subtitle, color):
        box = RoundedRectangle(width=2.55, height=1.2, corner_radius=0.12)
        box.set_stroke(color, width=3)
        box.set_fill(color, opacity=0.10)

        title_text = Text(title, font_size=20, weight=BOLD)
        subtitle_text = Text(subtitle, font_size=13, color=GRAY_B)
        title_text.move_to(box.get_center() + UP * 0.18)
        subtitle_text.move_to(box.get_center() + DOWN * 0.22)
        return VGroup(box, title_text, subtitle_text)

    def labeled_arrow(self, start, end, label, color):
        arrow = Arrow(start, end, buff=0.18, color=color, stroke_width=5)
        text = Text(label, font_size=13, color=color)
        text.next_to(arrow, UP, buff=0.08)
        return VGroup(arrow, text)

    def pulse_path(self, arrow_group, dot_color):
        arrow = arrow_group[0]
        dot = Dot(color=dot_color, radius=0.08).move_to(arrow.get_start())
        self.play(FadeIn(dot), run_time=0.15)
        self.play(MoveAlongPath(dot, arrow), run_time=0.85, rate_func=smooth)
        self.play(FadeOut(dot), run_time=0.15)

    def construct(self):
        self.camera.background_color = "#fbfbfb"

        title = Text("System Architecture Flow", font_size=36, weight=BOLD)
        subtitle = Text("Browser UI -> C Server -> ZeroMQ Broker -> C Worker -> Generated Program", font_size=18, color=GRAY_B)
        header = VGroup(title, subtitle).arrange(DOWN, buff=0.16)
        header.to_edge(UP, buff=0.35)
        self.play(Write(title), FadeIn(subtitle, shift=UP * 0.15))

        browser = self.make_node("Browser UI", "notebook page", BLUE_D)
        server = self.make_node("C HTTP Server", "REST API", TEAL_D)
        broker = self.make_node("ZeroMQ Broker", "ROUTER / DEALER", ORANGE)
        worker = self.make_node("C Kernel Worker", "compile + run", GREEN_D)
        runtime = self.make_node("Generated C", "binary + output", PURPLE_D)

        nodes = VGroup(browser, server, broker, worker, runtime).arrange(RIGHT, buff=0.28)
        nodes.move_to(UP * 0.55)

        request_arrows = [
            self.labeled_arrow(browser.get_right(), server.get_left(), "HTTP run", BLUE_D),
            self.labeled_arrow(server.get_right(), broker.get_left(), "RUN + JSON", TEAL_D),
            self.labeled_arrow(broker.get_right(), worker.get_left(), "shared queue", ORANGE),
            self.labeled_arrow(worker.get_right(), runtime.get_left(), "gcc + timeout", GREEN_D),
        ]

        response_runtime = runtime.copy().next_to(runtime, DOWN, buff=1.25)
        response_worker = worker.copy().next_to(worker, DOWN, buff=1.25)
        response_broker = broker.copy().next_to(broker, DOWN, buff=1.25)
        response_server = server.copy().next_to(server, DOWN, buff=1.25)
        response_browser = browser.copy().next_to(browser, DOWN, buff=1.25)
        response_nodes = VGroup(response_runtime, response_worker, response_broker, response_server, response_browser)

        response_arrows = [
            self.labeled_arrow(response_runtime.get_left(), response_worker.get_right(), "stdout / errors", PURPLE_D),
            self.labeled_arrow(response_worker.get_left(), response_broker.get_right(), "RESULT + JSON", GREEN_D),
            self.labeled_arrow(response_broker.get_left(), response_server.get_right(), "reply", ORANGE),
            self.labeled_arrow(response_server.get_left(), response_browser.get_right(), "display output", TEAL_D),
        ]

        self.play(LaggedStart(*[FadeIn(node, shift=UP * 0.2) for node in nodes], lag_ratio=0.12))

        step_label = Text("1. A user runs a notebook cell", font_size=24, color=BLUE_D)
        step_label.to_edge(DOWN, buff=0.45)
        self.play(Write(step_label))

        for arrow in request_arrows:
            self.play(Create(arrow[0]), FadeIn(arrow[1]), run_time=0.45)
            self.pulse_path(arrow, arrow[0].get_color())

        self.play(Transform(step_label, Text("2. The worker compiles and runs cumulative C code", font_size=24, color=GREEN_D).to_edge(DOWN, buff=0.45)))
        self.play(runtime.animate.set_fill(PURPLE_D, opacity=0.22).scale(1.05), run_time=0.45)
        self.play(runtime.animate.set_fill(PURPLE_D, opacity=0.10).scale(1 / 1.05), run_time=0.45)

        self.play(Transform(step_label, Text("3. Output returns through the same architecture", font_size=24, color=TEAL_D).to_edge(DOWN, buff=0.45)))
        self.play(LaggedStart(*[FadeIn(node, shift=DOWN * 0.15) for node in response_nodes], lag_ratio=0.08))

        for arrow in response_arrows:
            self.play(Create(arrow[0]), FadeIn(arrow[1]), run_time=0.45)
            self.pulse_path(arrow, arrow[0].get_color())

        final_note = Text("Complete notebook behavior: edit C cells, run them, and show output in the browser", font_size=21, color=BLACK)
        final_note.to_edge(DOWN, buff=0.35)
        self.play(Transform(step_label, final_note))
        self.wait(1.5)
