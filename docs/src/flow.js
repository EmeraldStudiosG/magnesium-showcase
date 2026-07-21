const FLOW_TYPES = [
  {
    attr: "does_not_compile",
    title: "This code does not compile!",
    img: "FlowAngry.png"
  },
  {
    attr: "not_desired_behavior",
    title: "This code does not produce the desired behavior.",
    img: "FlowAngry.png"
  },
  {
    attr: "good_practice",
    title: "This is good practice!",
    img: "FlowHappy.png"
  },
  {
    attr: "important",
    title: "This is important!",
    img: "FlowHappy.png"
  }
];

document.addEventListener("DOMContentLoaded", () => {
  for (let flowType of FLOW_TYPES) {
    attachFlows(flowType);
  }
});

function attachFlows(type) {
  let elements = document.getElementsByClassName(type.attr);

  for (let codeBlock of elements) {
    if (!(codeBlock instanceof HTMLElement)) continue;

    let codeLines = codeBlock.innerText;
    let extra = codeLines.endsWith("\n") ? 1 : 0;
    let numLines = codeLines.split("\n").length - extra;
    let size = numLines < 4 ? "small" : "large";

    let container = prepareFlowContainer(codeBlock, size === "small");
    if (!container) continue;

    container.appendChild(createFlow(type, size));
  }
}

function prepareFlowContainer(element, useButtons) {
  let foundButtons = element.parentElement?.querySelector(".buttons");
  if (useButtons && foundButtons) return foundButtons;

  let div = document.createElement("div");
  div.classList.add("flow-container");
  if (!element.parentElement) return null;
  element.parentElement.insertBefore(div, element);
  return div;
}

function createFlow(type, size) {
  let a = document.createElement("a");
  a.setAttribute("href", "index.html#flow");
  a.setAttribute("target", "_blank");

  let img = document.createElement("img");
  img.setAttribute("src", "assets/images/" + type.img);
  img.setAttribute("title", type.title);
  img.classList.add("flow");
  img.classList.add("flow-" + size);

  a.appendChild(img);
  return a;
}
