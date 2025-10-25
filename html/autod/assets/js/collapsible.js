const collapsibleCardBodies = new WeakMap();
const collapsibleCardButtons = new WeakMap();
let activeCollapsibleCard = null;
let collapsibleCardObserver = null;
let collapsibleCardsInitialized = false;
let collapsibleCardIdCounter = 0;

function ensureCollapsibleCardObserver(){
  if (!collapsibleCardObserver){
    collapsibleCardObserver = new MutationObserver(mutations => {
      for (const mutation of mutations){
        const target = mutation.target;
        if (!(target instanceof HTMLElement)) continue;
        const isHidden = target.hidden || target.style.display === 'none';
        if (isHidden){
          setCollapsibleCardExpanded(target, false);
        }
      }
    });
  }
  return collapsibleCardObserver;
}

function setCollapsibleCardExpanded(card, expanded){
  const body = collapsibleCardBodies.get(card);
  const button = collapsibleCardButtons.get(card);
  if (!body || !button) return;

  if (expanded){
    if (activeCollapsibleCard && activeCollapsibleCard !== card){
      setCollapsibleCardExpanded(activeCollapsibleCard, false);
    }
    card.classList.remove('collapsed');
    card.classList.add('expanded');
    button.setAttribute('aria-expanded', 'true');
    body.hidden = false;
    body.removeAttribute('aria-hidden');
    activeCollapsibleCard = card;
  } else {
    card.classList.add('collapsed');
    card.classList.remove('expanded');
    button.setAttribute('aria-expanded', 'false');
    body.hidden = true;
    body.setAttribute('aria-hidden', 'true');
    if (activeCollapsibleCard === card) activeCollapsibleCard = null;
  }
}

function toggleCollapsibleCard(card){
  const isExpanded = !card.classList.contains('collapsed');
  setCollapsibleCardExpanded(card, !isExpanded);
}

export function initCollapsibleCards(){
  if (collapsibleCardsInitialized) return;
  const cards = Array.from(document.querySelectorAll('main > section.card'));
  if (!cards.length){
    collapsibleCardsInitialized = true;
    return;
  }
  const observer = ensureCollapsibleCardObserver();
  for (const card of cards){
    const heading = card.querySelector(':scope > h2');
    const body = card.querySelector(':scope > .body');
    if (!heading || !body) continue;

    const label = (heading.textContent || '').replace(/\s+/g, ' ').trim() || card.getAttribute('aria-label') || card.id || 'Section';
    heading.textContent = '';

    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'card-toggle';

    const labelSpan = document.createElement('span');
    labelSpan.className = 'card-toggle-label';
    labelSpan.textContent = label;
    button.appendChild(labelSpan);

    const iconSpan = document.createElement('span');
    iconSpan.className = 'card-toggle-icon';
    iconSpan.setAttribute('aria-hidden', 'true');
    button.appendChild(iconSpan);

    let bodyId = body.id;
    if (!bodyId){
      const base = card.id ? `${card.id}-body` : `card-body-${++collapsibleCardIdCounter}`;
      bodyId = base;
      body.id = bodyId;
    }
    button.setAttribute('aria-controls', bodyId);
    button.setAttribute('aria-expanded', 'false');

    heading.appendChild(button);

    collapsibleCardBodies.set(card, body);
    collapsibleCardButtons.set(card, button);

    card.classList.add('collapsible-card', 'collapsed');
    body.hidden = true;
    body.setAttribute('aria-hidden', 'true');

    button.addEventListener('click', ev => {
      ev.preventDefault();
      ev.stopPropagation();
      toggleCollapsibleCard(card);
    });

    observer.observe(card, { attributes:true, attributeFilter:['style','hidden'] });
  }
  collapsibleCardsInitialized = true;
}
